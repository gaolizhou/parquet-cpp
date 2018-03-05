// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "parquet/file_writer.h"

#include "parquet/column_writer.h"
#include "parquet/schema-internal.h"
#include "parquet/schema.h"
#include "parquet/thrift.h"
#include "parquet/util/memory.h"

using arrow::MemoryPool;

using parquet::schema::GroupNode;
using parquet::schema::SchemaFlattener;

namespace parquet {

// FIXME: copied from reader-internal.cc
static constexpr uint8_t PARQUET_MAGIC[4] = {'P', 'A', 'R', '1'};

// ----------------------------------------------------------------------
// RowGroupWriter public API

RowGroupWriter::RowGroupWriter(std::unique_ptr<Contents> contents)
    : contents_(std::move(contents)) {}

void RowGroupWriter::Close() {
  if (contents_) {
    contents_->Close();
  }
}

ColumnWriter* RowGroupWriter::NextColumn(const std::map<std::string, std::string> &meta) {
  return contents_->NextColumn(meta);
}

int RowGroupWriter::current_column() { return contents_->current_column(); }

int RowGroupWriter::num_columns() const { return contents_->num_columns(); }

int64_t RowGroupWriter::num_rows() const { return contents_->num_rows(); }

// ----------------------------------------------------------------------
// RowGroupSerializer

// RowGroupWriter::Contents implementation for the Parquet file specification
class RowGroupSerializer : public RowGroupWriter::Contents {
 public:
  RowGroupSerializer(OutputStream* sink, RowGroupMetaDataBuilder* metadata,
                     const WriterProperties* properties)
      : sink_(sink),
        metadata_(metadata),
        properties_(properties),
        total_bytes_written_(0),
        closed_(false),
        current_column_index_(0),
        num_rows_(-1) {}

  int num_columns() const override { return metadata_->num_columns(); }

  int64_t num_rows() const override {
    if (current_column_writer_) {
      CheckRowsWritten();
    }
    return num_rows_ < 0 ? 0 : num_rows_;
  }

  ColumnWriter* NextColumn(const std::map<std::string, std::string> &meta) override {
    if (current_column_writer_) {
      CheckRowsWritten();
    }

    // Throws an error if more columns are being written
    auto col_meta = metadata_->NextColumnChunk(meta);

    if (current_column_writer_) {
      total_bytes_written_ += current_column_writer_->Close();
    }

    ++current_column_index_;

    const ColumnDescriptor* column_descr = col_meta->descr();
    std::unique_ptr<PageWriter> pager =
        PageWriter::Open(sink_, properties_->compression(column_descr->path()), col_meta,
                         properties_->memory_pool());
    current_column_writer_ = ColumnWriter::Make(col_meta, std::move(pager), properties_);
    return current_column_writer_.get();
  }

  int current_column() const override { return metadata_->current_column(); }

  void Close() override {
    if (!closed_) {
      closed_ = true;

      if (current_column_writer_) {
        CheckRowsWritten();
        total_bytes_written_ += current_column_writer_->Close();
        current_column_writer_.reset();
      }

      // Ensures all columns have been written
      metadata_->Finish(total_bytes_written_);
    }
  }

 private:
  OutputStream* sink_;
  mutable RowGroupMetaDataBuilder* metadata_;
  const WriterProperties* properties_;
  int64_t total_bytes_written_;
  bool closed_;
  int current_column_index_;
  mutable int64_t num_rows_;

  void CheckRowsWritten() const {
    int64_t current_rows = current_column_writer_->rows_written();
    if (num_rows_ < 0) {
      num_rows_ = current_rows;
      metadata_->set_num_rows(current_rows);
    } else if (num_rows_ != current_rows) {
      std::stringstream ss;
      ss << "Column " << current_column_index_ << " had " << current_rows
         << " while previous column had " << num_rows_;
      throw ParquetException(ss.str());
    }
  }

  std::shared_ptr<ColumnWriter> current_column_writer_;
};

// ----------------------------------------------------------------------
// FileSerializer

// An implementation of ParquetFileWriter::Contents that deals with the Parquet
// file structure, Thrift serialization, and other internal matters

class FileSerializer : public ParquetFileWriter::Contents {
 public:
  static std::unique_ptr<ParquetFileWriter::Contents> Open(
      const std::shared_ptr<OutputStream>& sink, const std::shared_ptr<GroupNode>& schema,
      const std::shared_ptr<WriterProperties>& properties,
      const std::shared_ptr<const KeyValueMetadata>& key_value_metadata) {
    std::unique_ptr<ParquetFileWriter::Contents> result(
        new FileSerializer(sink, schema, properties, key_value_metadata));

    return result;
  }

  void Close() override {
    if (is_open_) {
      if (row_group_writer_) {
        num_rows_ += row_group_writer_->num_rows();
        row_group_writer_->Close();
      }
      row_group_writer_.reset();

      // Write magic bytes and metadata
      WriteMetaData();

      sink_->Close();
      is_open_ = false;
    }
  }

  int num_columns() const override { return schema_.num_columns(); }

  int num_row_groups() const override { return num_row_groups_; }

  int64_t num_rows() const override { return num_rows_; }

  const std::shared_ptr<WriterProperties>& properties() const override {
    return properties_;
  }

  RowGroupWriter* AppendRowGroup() override {
    if (row_group_writer_) {
      row_group_writer_->Close();
    }
    num_row_groups_++;
    auto rg_metadata = metadata_->AppendRowGroup();
    std::unique_ptr<RowGroupWriter::Contents> contents(
        new RowGroupSerializer(sink_.get(), rg_metadata, properties_.get()));
    row_group_writer_.reset(new RowGroupWriter(std::move(contents)));
    return row_group_writer_.get();
  }

  virtual ~FileSerializer() override {
    try {
      Close();
    } catch (...) {
    }
  }

 private:
  friend class ZdbFileSerializer;
  FileSerializer(const std::shared_ptr<OutputStream>& sink,
                 const std::shared_ptr<GroupNode>& schema,
                 const std::shared_ptr<WriterProperties>& properties,
                 const std::shared_ptr<const KeyValueMetadata>& key_value_metadata)
      : ParquetFileWriter::Contents(schema, key_value_metadata),
        sink_(sink),
        is_open_(true),
        properties_(properties),
        num_row_groups_(0),
        num_rows_(0),
        metadata_(FileMetaDataBuilder::Make(&schema_, properties, key_value_metadata)) {
    StartFile();
  }

  std::shared_ptr<OutputStream> sink_;
  bool is_open_;
  const std::shared_ptr<WriterProperties> properties_;
  int num_row_groups_;
  int64_t num_rows_;
  std::unique_ptr<FileMetaDataBuilder> metadata_;
  std::unique_ptr<RowGroupWriter> row_group_writer_;

  void StartFile() {
    // Parquet files always start with PAR1
    sink_->Write(PARQUET_MAGIC, 4);
  }

  virtual void WriteMetaData() {
    // Write MetaData
    uint32_t metadata_len = static_cast<uint32_t>(sink_->Tell());

    // Get a FileMetaData
    auto metadata = metadata_->Finish();
    metadata->WriteTo(sink_.get());
    metadata_len = static_cast<uint32_t>(sink_->Tell()) - metadata_len;

    // Write Footer
    sink_->Write(reinterpret_cast<uint8_t*>(&metadata_len), 4);
    sink_->Write(PARQUET_MAGIC, 4);
  }
};



class ZdbFileSerializer : public FileSerializer {
 public:
  static std::unique_ptr<ParquetFileWriter::Contents> Open(
      const std::shared_ptr<OutputStream> &sink,
      const std::shared_ptr<GroupNode> &schema,
      const std::shared_ptr<WriterProperties> &properties,
      const std::shared_ptr<const KeyValueMetadata> &key_value_metadata,
      std::shared_ptr<parquet::FileMetaData> old_file_meta = nullptr) {
    std::unique_ptr<ParquetFileWriter::Contents> result(
        new ZdbFileSerializer(sink, schema, properties, key_value_metadata, old_file_meta));

    return result;
  }

  virtual void WriteMetaData() {
    //modify meta
    // Write MetaData
    uint32_t metadata_len = static_cast<uint32_t>(sink_->Tell());

    // Get a FileMetaData
    auto metadata = metadata_->Finish();

    auto metadata_target = metadata->GetFormatFileMetaData();
    auto metadata_old = old_file_meta_->GetFormatFileMetaData();

    metadata_target->row_groups.insert(metadata_target->row_groups.begin(),
                                       metadata_old->row_groups.begin(),
                                       metadata_old->row_groups.end());
    metadata_target->num_rows += metadata_old->num_rows;

    //FIXME key_value_metadata
    std::map<std::string, std::string> kv_meta;
    std::transform(metadata_old->key_value_metadata.begin(),
                   metadata_old->key_value_metadata.end(),
                   std::inserter(kv_meta, std::begin(kv_meta)),
                   [](const format::KeyValue &kv){
                     return std::make_pair(kv.key, kv.value);
                   });
    std::transform(metadata_target->key_value_metadata.begin(),
                   metadata_target->key_value_metadata.end(),
                   std::inserter(kv_meta, std::begin(kv_meta)),
                   [](const format::KeyValue &kv){
                     return std::make_pair(kv.key, kv.value);
                   });

    metadata_target->key_value_metadata.clear();

    std::transform(kv_meta.begin(), kv_meta.end(),
                   std::back_inserter(metadata_target->key_value_metadata),
    [](const std::pair<std::string, std::string> &kv){
      format::KeyValue key_value;
      key_value.key = kv.first;
      key_value.__isset.value = true;
      key_value.value = kv.second;
      return key_value;
    });

    if (!metadata_target->key_value_metadata.empty()) {
      metadata_target->__isset.key_value_metadata = true;
    } else {
      metadata_target->__isset.key_value_metadata = false;
    }

    auto new_meta = FileMetaData::Make(metadata_target);

    new_meta->WriteTo(sink_.get());
    metadata_len = static_cast<uint32_t>(sink_->Tell()) - metadata_len;

    // Write Footer
    sink_->Write(reinterpret_cast<uint8_t*>(&metadata_len), 4);
    sink_->Write(PARQUET_MAGIC, 4);
  }
 private:
  ZdbFileSerializer(const std::shared_ptr<OutputStream> &sink,
                    const std::shared_ptr<GroupNode> &schema,
                    const std::shared_ptr<WriterProperties> &properties,
                    const std::shared_ptr<const KeyValueMetadata> &key_value_metadata,
                    std::shared_ptr<parquet::FileMetaData> old_file_meta)
      : FileSerializer(sink, schema, properties, key_value_metadata), old_file_meta_(old_file_meta) {
    if (!old_file_meta_) {
      StartFile();
      return;
    }
  }


  std::shared_ptr<parquet::FileMetaData> old_file_meta_;
};


// ----------------------------------------------------------------------
// ParquetFileWriter public API

ParquetFileWriter::ParquetFileWriter() {}

ParquetFileWriter::~ParquetFileWriter() {
  try {
    Close();
  } catch (...) {
  }
}

std::unique_ptr<ParquetFileWriter> ParquetFileWriter::Open(
    const std::shared_ptr<::arrow::io::OutputStream>& sink,
    const std::shared_ptr<GroupNode>& schema,
    const std::shared_ptr<WriterProperties>& properties,
    const std::shared_ptr<const KeyValueMetadata>& key_value_metadata) {
  return Open(std::make_shared<ArrowOutputStream>(sink), schema, properties,
              key_value_metadata);
}
std::unique_ptr<ParquetFileWriter> ParquetFileWriter::OpenAppend(
    const std::shared_ptr<::arrow::io::OutputStream>& sink,
    const std::shared_ptr<schema::GroupNode>& schema,
    std::shared_ptr<parquet::FileMetaData> old_file_meta,
    const std::shared_ptr<WriterProperties>& properties,
    const std::shared_ptr<const KeyValueMetadata>& key_value_metadata) {
  auto contents = ZdbFileSerializer::Open(std::make_shared<ArrowOutputStream>(sink), schema,
                                          properties, key_value_metadata, old_file_meta);
  std::unique_ptr<ParquetFileWriter> result(new ParquetFileWriter());
  result->Open(std::move(contents));
  return result;
}

std::unique_ptr<ParquetFileWriter> ParquetFileWriter::Open(
    const std::shared_ptr<OutputStream>& sink,
    const std::shared_ptr<schema::GroupNode>& schema,
    const std::shared_ptr<WriterProperties>& properties,
    const std::shared_ptr<const KeyValueMetadata>& key_value_metadata) {
  auto contents = FileSerializer::Open(sink, schema, properties, key_value_metadata);
  std::unique_ptr<ParquetFileWriter> result(new ParquetFileWriter());
  result->Open(std::move(contents));
  return result;
}

const SchemaDescriptor* ParquetFileWriter::schema() const { return contents_->schema(); }

const ColumnDescriptor* ParquetFileWriter::descr(int i) const {
  return contents_->schema()->Column(i);
}

int ParquetFileWriter::num_columns() const { return contents_->num_columns(); }

int64_t ParquetFileWriter::num_rows() const { return contents_->num_rows(); }

int ParquetFileWriter::num_row_groups() const { return contents_->num_row_groups(); }

const std::shared_ptr<const KeyValueMetadata>& ParquetFileWriter::key_value_metadata()
    const {
  return contents_->key_value_metadata();
}

void ParquetFileWriter::Open(std::unique_ptr<ParquetFileWriter::Contents> contents) {
  contents_ = std::move(contents);
}

void ParquetFileWriter::Close() {
  if (contents_) {
    contents_->Close();
    contents_.reset();
  }
}

RowGroupWriter* ParquetFileWriter::AppendRowGroup() {
  return contents_->AppendRowGroup();
}

RowGroupWriter* ParquetFileWriter::AppendRowGroup(int64_t num_rows) {
  return AppendRowGroup();
}

const std::shared_ptr<WriterProperties>& ParquetFileWriter::properties() const {
  return contents_->properties();
}



}  // namespace parquet
