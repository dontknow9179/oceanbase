/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#include <gtest/gtest.h>
#include <thread>
#define private public
#define protected public
#include "libobtable.h"
#include "lib/utility/ob_test_util.h"
#include "lib/allocator/page_arena.h"
#include "lib/lds/ob_lds_constructor.hpp"
#include "share/table/ob_table_rpc_struct.h"
#include "common/row/ob_row.h"
#include "observer/table/ob_htable_utils.h"
#include "observer/table/ob_htable_filter_operator.h"
#include "observer/table/ob_table_service.h"
#include <thread>
#undef private
#undef protected
using namespace oceanbase::common;
using namespace oceanbase::table;

const char* host = "100.88.11.91";
int32_t sql_port = 60809;
int32_t rpc_port = 60808;
const char* tenant = "sys";
const char* user_name = "root";
const char* passwd = "";
const char* db = "test";
const char* table_name = "batch_execute_test";
const char* sys_root_pass = "";
typedef char DefaultBuf[128];

// create table if not exists batch_execute_test (C1 bigint primary key, C2 bigint, C3 varchar(100)) PARTITION BY KEY(C1) PARTITIONS 16
class TestBatchExecute: public ::testing::Test
{
public:
  TestBatchExecute();
  virtual ~TestBatchExecute();
  virtual void SetUp();
  virtual void TearDown();
private:
  // disallow copy
  DISALLOW_COPY_AND_ASSIGN(TestBatchExecute);
protected:
  // function members
  void generate_get(ObTableQuery &query, ObObj pk_objs_start[], ObObj pk_objs_end[], const char* rowkey);
  void prepare_data(ObTable *the_table);
protected:
  static const int64_t BATCH_SIZE;
  ObTableServiceClient* service_client_ = NULL;
  ObTable* table_ = NULL;
};

const int64_t TestBatchExecute::BATCH_SIZE = 100;
TestBatchExecute::TestBatchExecute()
{
}

TestBatchExecute::~TestBatchExecute()
{
}

void TestBatchExecute::SetUp()
{
  ASSERT_TRUE(NULL == service_client_);
  service_client_ = ObTableServiceClient::alloc_client();
  ASSERT_TRUE(NULL != service_client_);
  int ret = service_client_->init(ObString::make_string(host), sql_port, rpc_port,
                                  ObString::make_string(tenant), ObString::make_string(user_name),
                                  ObString::make_string(passwd), ObString::make_string(db),
                                  ObString::make_string(sys_root_pass));
  ASSERT_EQ(OB_SUCCESS, ret);
  ret = service_client_->alloc_table(ObString::make_string(table_name), table_);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableRequestOptions options;
  options.set_returning_affected_rows(true);
  //options.set_consistency_level(ObTableConsistencyLevel::EVENTUAL);
  table_->set_default_request_options(options);
}

void TestBatchExecute::TearDown()
{
  if (NULL != table_) {
    ObTableEntityFactory<ObTableEntity> entity_factory;
    ObTableBatchOperation batch_operation;
    ObITableEntity *entity = NULL;
    for (int64_t i = 0; i < BATCH_SIZE*2; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));
    }
    ASSERT_TRUE(!batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());

    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    ASSERT_EQ(BATCH_SIZE*2, result.count());
  }
  service_client_->free_table(table_);
  table_ = NULL;
  ObTableServiceClient::free_client(service_client_);
  service_client_ = NULL;
}


ObString C1 = ObString::make_string("C1");
ObString C2 = ObString::make_string("C2");
ObString C3 = ObString::make_string("C3");


TEST_F(TestBatchExecute, entity_factory)
{
  ObTableEntityFactory<ObTableEntity> entity_factory;
  static const int64_t N = 100;
  static const int64_t R = 3;
  for (int round = 0; round < R; ++round) {

    for (int i = 0; i < N; ++i)
    {
      ObITableEntity *entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
    } // end for
    fprintf(stderr, "used=%ld free=%ld mem_total=%ld mem_used=%ld\n",
            entity_factory.get_used_count(), entity_factory.get_free_count(),
            entity_factory.get_total_mem(), entity_factory.get_used_mem());
    entity_factory.free_and_reuse();
    fprintf(stderr, "used=%ld free=%ld mem_total=%ld mem_used=%ld\n",
            entity_factory.get_used_count(), entity_factory.get_free_count(),
            entity_factory.get_total_mem(), entity_factory.get_used_mem());
  }
}

TEST_F(TestBatchExecute, serialize_batch_result)
{
  ObTableBatchOperationResult result;
  ObTableEntity result_entity;
  ObTableOperationResult single_op_result;
  single_op_result.set_entity(result_entity);
  single_op_result.set_errno(1234);
  single_op_result.set_type(ObTableOperationType::INSERT_OR_UPDATE);
  single_op_result.set_affected_rows(4321);
  ASSERT_EQ(OB_SUCCESS, result.push_back(single_op_result));
  int64_t expected_len = result.get_serialize_size();
  char buf[1024];
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, result.serialize(buf, 1024, pos));
  ASSERT_EQ(expected_len, pos);

  ObTableBatchOperationResult result2;
  ObTableEntityFactory<ObTableEntity> entity_factory;
  result2.set_entity_factory(&entity_factory);
  int64_t data_len = pos;
  //fprintf(stderr, "yzfdebug datalen=%ld expectedlen=%ld\n", data_len, expected_len);
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, result2.deserialize(buf, data_len, pos));
  ASSERT_EQ(1, result2.count());
  ASSERT_EQ(1234, result2.at(0).get_errno());
  ASSERT_EQ(4321, result2.at(0).get_affected_rows());
  ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, result2.at(0).type());
}

//TEST_F(TestBatchExecute, all_single_operation)
//{
  //OB_LOG(INFO, "begin all_single_operation");
  //// insert C2
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObITableEntity *entity = NULL;
  //entity = entity_factory.alloc();
  //ASSERT_TRUE(NULL != entity);
  //int64_t key_key = 139107;
  //ObObj key;
  //key.set_int(key_key);
  //int64_t value_value = 33521;
  //ObObj value;
  //value.set_int(value_value);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  //ObString c3_value = ObString::make_string("hello world");
  //ObTableOperation table_operation = ObTableOperation::insert(*entity);
  //ObTableOperationResult r;
  //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
  //const ObITableEntity *result_entity = NULL;
  //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //ASSERT_EQ(1, r.get_affected_rows());
  //ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  //ASSERT_TRUE(NULL != result_entity);
  //ASSERT_TRUE(result_entity->is_empty());
  //// insert again: fail
  //{
    //ObTableOperation table_operation = ObTableOperation::insert(*entity);
    //ObTableOperationResult r;
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_ERR_PRIMARY_KEY_DUPLICATE, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(value_value, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ASSERT_TRUE(value.is_null());
  //}
  //// update C3
  //{
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //ObTableOperation table_operation = ObTableOperation::update(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(value_value, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ASSERT_TRUE(str == c3_value);
  //}
  //// update C3 not exist key
  //{
    //entity->reset();
    //ObObj not_exist_key;
    //not_exist_key.set_int(key_key+1);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(not_exist_key));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //ObTableOperation table_operation = ObTableOperation::update(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// replace C3
  //{
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //ObTableOperation table_operation = ObTableOperation::replace(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(2, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_TRUE(value.is_null());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ASSERT_TRUE(str == c3_value);
  //}
  //// insert_or_update C2: update
  //{
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_int(value_value);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //ObTableOperation table_operation = ObTableOperation::insert_or_update(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(value_value, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ASSERT_TRUE(str == c3_value);
  //}
  //// delete not exist row
  //{
    //entity->reset();
    //ObObj not_exist_key;
    //not_exist_key.set_int(key_key+1);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(not_exist_key));
    //ObTableOperation table_operation = ObTableOperation::del(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::DEL, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// delete
  //{
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ObTableOperation table_operation = ObTableOperation::del(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::DEL, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// get again
  //{
    //ObObj null_obj;
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// insert_or_update C2: insert
  //{
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_int(value_value);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //ObTableOperation table_operation = ObTableOperation::insert_or_update(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(value_value, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ASSERT_TRUE(value.is_null());
  //}
  //// delete & cleanup
  //{
    //entity->reset();
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ObTableOperation table_operation = ObTableOperation::del(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::DEL, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
//}

//TEST_F(TestBatchExecute, multi_insert_or_update_AND_multi_get)
//{
  //OB_LOG(INFO, "begin multi_insert_or_update");
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObTableBatchOperation batch_operation;
  //ObITableEntity *entity = NULL;
  //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    //entity = entity_factory.alloc();
    //ASSERT_TRUE(NULL != entity);
    //ObObj key;
    //key.set_int(i*2);
    //ObObj value;
    //value.set_int(100+i);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
  //}
  //ASSERT_TRUE(!batch_operation.is_readonly());
  //ASSERT_TRUE(batch_operation.is_same_type());
  //ASSERT_TRUE(batch_operation.is_same_properties_names());
  //ObTableBatchOperationResult result;
  //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  //OB_LOG(INFO, "batch execute result", K(result));
  //ASSERT_EQ(BATCH_SIZE, result.count());
  //for (int64_t i = 0; i < BATCH_SIZE; ++i)
  //{
    //const ObTableOperationResult &r = result.at(i);
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, r.type());
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //} // end for

  //// get and verify
  //const ObITableEntity *result_entity = NULL;
  //{
    //batch_operation.reset();
    //entity_factory.free_and_reuse();
    //ObObj null_obj;
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //entity = entity_factory.alloc();
      //ASSERT_TRUE(NULL != entity);
      //ObObj key;
      //key.set_int(i*2);
      //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    //}
    //ASSERT_TRUE(batch_operation.is_readonly());
    //ASSERT_TRUE(batch_operation.is_same_type());
    //ASSERT_TRUE(batch_operation.is_same_properties_names());
    //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    //OB_LOG(INFO, "batch execute result", K(result));
    //ASSERT_EQ(BATCH_SIZE, result.count());
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //const ObTableOperationResult &r = result.at(i);
      //ASSERT_EQ(OB_SUCCESS, r.get_errno());
      //ASSERT_EQ(ObTableOperationType::GET, r.type());
      //ASSERT_EQ(0, r.get_affected_rows());
      //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      //ASSERT_EQ(0, result_entity->get_rowkey_size());
      //ObObj value;
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      //ASSERT_EQ(100+i, value.get_int());
    //}
  //}
  //// multi-get case 2
  //{
    //ObObj null_obj;
    //batch_operation.reset();
    //entity_factory.free_and_reuse();
    //for (int64_t i = BATCH_SIZE-1; i >= 0; --i) {
      //entity = entity_factory.alloc();
      //ASSERT_TRUE(NULL != entity);
      //ObObj key;
      //key.set_int(i);
      //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      //ASSERT_EQ(OB_SUCCESS, entity->add_retrieve_property(C2));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    //}
    //ASSERT_TRUE(batch_operation.is_readonly());
    //ASSERT_TRUE(batch_operation.is_same_type());
    //ASSERT_TRUE(batch_operation.is_same_properties_names());
    //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    //OB_LOG(INFO, "batch execute result", K(result));
    //result_entity = NULL;
    //ASSERT_EQ(BATCH_SIZE, result.count());
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //const ObTableOperationResult &r = result.at(i);
      //ASSERT_EQ(ObTableOperationType::GET, r.type());
      //if (i % 2 == 1) {
        //ASSERT_EQ(OB_SUCCESS, r.get_errno());
        //ASSERT_EQ(0, r.get_affected_rows());
        //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
        //ASSERT_EQ(0, result_entity->get_rowkey_size());
        //ObObj value;
        //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
        ////fprintf(stderr, "get value i=%ld v=%s\n", i, S(value));
        //ASSERT_EQ(100+(BATCH_SIZE-1-i)/2, value.get_int());
      //} else {
        //ASSERT_EQ(OB_SUCCESS, r.get_errno());
        //ASSERT_EQ(0, r.get_affected_rows());
        //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
        //ASSERT_TRUE(result_entity->is_empty());
      //}
    //}
  //}
//}

TEST_F(TestBatchExecute, multi_insert)
{
  OB_LOG(INFO, "begin multi_insert");
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i*2);
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  for (int64_t i = 0; i < BATCH_SIZE; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    ASSERT_EQ(1, r.get_affected_rows());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  }
  // get and verify
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      ObObj value;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      ASSERT_EQ(100+i, value.get_int());
    }
  }
  // insert again
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2);
      ObObj value;
      value.set_int(100+i);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
      ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
    }
    ASSERT_TRUE(!batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(OB_ERR_PRIMARY_KEY_DUPLICATE, r.get_errno());
      ASSERT_EQ(ObTableOperationType::INSERT, r.type());
      const ObITableEntity *result_entity = NULL;
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_TRUE(result_entity->is_empty());
    }
  }
}

TEST_F(TestBatchExecute, multi_replace)
{
  OB_LOG(INFO, "begin multi_replace");
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i*2);
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.replace(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  }
  // get and verify
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      ObObj value;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      ASSERT_EQ(100+i, value.get_int());
    }
  }
  // replace again
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2);
      ObObj value;
      value.set_int(200+i);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
      ASSERT_EQ(OB_SUCCESS, batch_operation.replace(*entity));
    }
    ASSERT_TRUE(!batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(2, r.get_affected_rows());
      ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
      const ObITableEntity *result_entity = NULL;
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_TRUE(result_entity->is_empty());
    }
  }
  // get and verify
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      ObObj value;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      ASSERT_EQ(200+i, value.get_int());
    }
  }
}

//TEST_F(TestBatchExecute, multi_update)
//{
  //OB_LOG(INFO, "begin multi_update");
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObTableBatchOperation batch_operation;
  //// prepare data
  //ObITableEntity *entity = NULL;
  //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    //entity = entity_factory.alloc();
    //ASSERT_TRUE(NULL != entity);
    //ObObj key;
    //key.set_int(i*2);
    //ObObj value;
    //value.set_int(100+i);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //ASSERT_EQ(OB_SUCCESS, batch_operation.replace(*entity));
  //}
  //ASSERT_TRUE(!batch_operation.is_readonly());
  //ASSERT_TRUE(batch_operation.is_same_type());
  //ASSERT_TRUE(batch_operation.is_same_properties_names());
  //ObTableBatchOperationResult result;
  //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  //OB_LOG(INFO, "batch execute result", K(result));
  //ASSERT_EQ(BATCH_SIZE, result.count());
  //for (int64_t i = 0; i < BATCH_SIZE; ++i)
  //{
    //const ObTableOperationResult &r = result.at(i);
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
    //ASSERT_EQ(1, r.get_affected_rows());
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(result_entity->is_empty());
  //}
  //ObString c3_value = "c3_value";
  //// update C3
  //{
    //batch_operation.reset();
    //entity_factory.free_and_reuse();
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //entity = entity_factory.alloc();
      //ASSERT_TRUE(NULL != entity);
      //ObObj key;
      //key.set_int(i*2);
      //ObObj value;
      //value.set_varchar(c3_value);
      //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.update(*entity));
    //}
    //ASSERT_TRUE(!batch_operation.is_readonly());
    //ASSERT_TRUE(batch_operation.is_same_type());
    //ASSERT_TRUE(batch_operation.is_same_properties_names());
    //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    //ASSERT_EQ(BATCH_SIZE, result.count());
    //for (int64_t i = 0; i < BATCH_SIZE; ++i)
    //{
      //const ObTableOperationResult &r = result.at(i);
      //ASSERT_EQ(OB_SUCCESS, r.get_errno());
      //ASSERT_EQ(1, r.get_affected_rows());
      //ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
      //const ObITableEntity *result_entity = NULL;
      //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      //ASSERT_TRUE(result_entity->is_empty());
    //}
  //}
  //// get and verify
  //{
    //batch_operation.reset();
    //entity_factory.free_and_reuse();
    //ObObj null_obj;
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //entity = entity_factory.alloc();
      //ASSERT_TRUE(NULL != entity);
      //ObObj key;
      //key.set_int(i*2);
      //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    //}
    //ASSERT_TRUE(batch_operation.is_readonly());
    //ASSERT_TRUE(batch_operation.is_same_type());
    //ASSERT_TRUE(batch_operation.is_same_properties_names());
    //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    //OB_LOG(INFO, "batch execute result", K(result));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(BATCH_SIZE, result.count());
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //const ObTableOperationResult &r = result.at(i);
      //ASSERT_EQ(OB_SUCCESS, r.get_errno());
      //ASSERT_EQ(0, r.get_affected_rows());
      //ASSERT_EQ(ObTableOperationType::GET, r.type());
      //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      //ASSERT_EQ(0, result_entity->get_rowkey_size());
      //ObObj value;
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      //ASSERT_EQ(100+i, value.get_int());
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
      //ObString str;
      //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
      //ASSERT_TRUE(str == c3_value);
    //}
  //}
  //// update half of the rows
  //{
    //batch_operation.reset();
    //entity_factory.free_and_reuse();
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //entity = entity_factory.alloc();
      //ASSERT_TRUE(NULL != entity);
      //ObObj key;
      //key.set_int(i);
      //ObObj value;
      //value.set_int(200+i);
      //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.update(*entity));
    //}
    //ASSERT_TRUE(!batch_operation.is_readonly());
    //ASSERT_TRUE(batch_operation.is_same_type());
    //ASSERT_TRUE(batch_operation.is_same_properties_names());
    //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    //ASSERT_EQ(BATCH_SIZE, result.count());
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //const ObTableOperationResult &r = result.at(i);
      //ASSERT_EQ(OB_SUCCESS, r.get_errno());
      //ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
      //const ObITableEntity *result_entity = NULL;
      //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      //ASSERT_TRUE(result_entity->is_empty());
      //if (i % 2 == 0) {
        //ASSERT_EQ(1, r.get_affected_rows());
      //} else {
        //ASSERT_EQ(0, r.get_affected_rows());
      //}
    //}
  //}
  //// get and verify
  //{
    //batch_operation.reset();
    //entity_factory.free_and_reuse();
    //ObObj null_obj;
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //entity = entity_factory.alloc();
      //ASSERT_TRUE(NULL != entity);
      //ObObj key;
      //key.set_int(i*2);
      //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    //}
    //ASSERT_TRUE(batch_operation.is_readonly());
    //ASSERT_TRUE(batch_operation.is_same_type());
    //ASSERT_TRUE(batch_operation.is_same_properties_names());
    //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    //OB_LOG(INFO, "batch execute result", K(result));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(BATCH_SIZE, result.count());
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //const ObTableOperationResult &r = result.at(i);
      //ASSERT_EQ(OB_SUCCESS, r.get_errno());
      //ASSERT_EQ(0, r.get_affected_rows());
      //ASSERT_EQ(ObTableOperationType::GET, r.type());
      //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      //ASSERT_EQ(0, result_entity->get_rowkey_size());
      //ObObj value;
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      //if (i < BATCH_SIZE/2) {
        //ASSERT_EQ(200+i*2, value.get_int());
      //} else {
        //ASSERT_EQ(100+i, value.get_int());
      //}
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
      //ObString str;
      //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
      //ASSERT_TRUE(str == c3_value);
    //}
  //}
//}


TEST_F(TestBatchExecute, multi_delete)
{
  OB_LOG(INFO, "begin multi_delete");
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  // prepare data
  ObITableEntity *entity = NULL;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i*2);
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.replace(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  for (int64_t i = 0; i < BATCH_SIZE; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
    ASSERT_EQ(1, r.get_affected_rows());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  }

  // delete half of the rows
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));
    }
    ASSERT_TRUE(!batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(ObTableOperationType::DEL, r.type());
      const ObITableEntity *result_entity = NULL;
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_TRUE(result_entity->is_empty());
      if (i % 2 == 0) {
        ASSERT_EQ(1, r.get_affected_rows());
      } else {
        ASSERT_EQ(0, r.get_affected_rows());
      }
    }
  }
  // get and verify
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(BATCH_SIZE, result.count());
    ObObj value;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      if (i < BATCH_SIZE/2) {
        ASSERT_TRUE(result_entity->is_empty());
      } else {
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
        ASSERT_EQ(100+i, value.get_int());
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
        ASSERT_TRUE(value.is_null());
      }
    }
  }
}


TEST_F(TestBatchExecute, multi_get_complex)
{
  OB_LOG(INFO, "begin multi_get complex");
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  // prepare data
  ObITableEntity *entity = NULL;
  ObString c3_value = "c3_value";
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i*2);
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    value.set_varchar(c3_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.replace(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  const ObITableEntity *result_entity = NULL;
  for (int64_t i = 0; i < BATCH_SIZE; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  }
  // complex batch get and verify
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      if (i % 2 == 1) {
        ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      } else {
        ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
      }
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(!batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      ObObj value;
      ObString str;
      if (i % 2 == 1) {
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
        ASSERT_TRUE(OB_SUCCESS != result_entity->get_property(C3, value));
        ASSERT_EQ(100+i, value.get_int());
      } else {
        ASSERT_TRUE(OB_SUCCESS != result_entity->get_property(C2, value));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
        ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
        ASSERT_TRUE(str == c3_value);
      }
    }
  }
}

TEST_F(TestBatchExecute, complex_batch_execute)
{
  OB_LOG(INFO, "begin complex_batch_execute");
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  // prepare data
  ObITableEntity *entity = NULL;
  ObString c3_value = "c3_value";
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i);
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    value.set_varchar(c3_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.replace(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  }

  {
    // complex batch execute with hybrid operations types
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    ObObj value;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      switch (i % 6) {
        case 0:  // get
          ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
          ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
          ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
          break;
        case 1:  // insert
          value.set_int(200+i);
          ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
          ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
          break;
        case 2:  // delete
          ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));
          break;
        case 3:  // update
          value.set_int(300+i);
          ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
          ASSERT_EQ(OB_SUCCESS, batch_operation.update(*entity));
          break;
        case 4:  // insert_or_update
          value.set_int(400+i);
          ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
          ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
          break;
        case 5:  // replace
          value.set_int(500+i);
          ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
          ASSERT_EQ(OB_SUCCESS, batch_operation.replace(*entity));
          break;
        default:
          ASSERT_TRUE(0);
          break;
      }
    }
    ASSERT_TRUE(!batch_operation.is_readonly());
    ASSERT_TRUE(!batch_operation.is_same_type());
    ASSERT_TRUE(!batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    ASSERT_EQ(BATCH_SIZE, result.count());
    const ObITableEntity *result_entity = NULL;
    ObString str;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      switch (i % 6) {
        case 0:  // get
          ASSERT_EQ(OB_SUCCESS, r.get_errno());
          ASSERT_EQ(0, r.get_affected_rows());
          ASSERT_EQ(ObTableOperationType::GET, r.type());
          ASSERT_EQ(0, result_entity->get_rowkey_size());
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
          ASSERT_EQ(100+i, value.get_int());
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
          ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
          ASSERT_TRUE(str == c3_value);
          break;
        case 1:  // insert
          ASSERT_EQ(OB_ERR_PRIMARY_KEY_DUPLICATE, r.get_errno());
          ASSERT_EQ(0, r.get_affected_rows());
          ASSERT_EQ(ObTableOperationType::INSERT, r.type());
          ASSERT_TRUE(result_entity->is_empty());
          break;
        case 2:  // delete
          ASSERT_EQ(OB_SUCCESS, r.get_errno());
          ASSERT_EQ(1, r.get_affected_rows());
          ASSERT_EQ(ObTableOperationType::DEL, r.type());
          ASSERT_TRUE(result_entity->is_empty());
          break;
        case 3:  // update
          ASSERT_EQ(OB_SUCCESS, r.get_errno());
          ASSERT_EQ(1, r.get_affected_rows());
          ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
          ASSERT_TRUE(result_entity->is_empty());
          break;
        case 4:  // insert_or_update
          ASSERT_EQ(OB_SUCCESS, r.get_errno());
          ASSERT_EQ(1, r.get_affected_rows());
          ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, r.type());
          ASSERT_TRUE(result_entity->is_empty());
          break;
        case 5:  // replace
          ASSERT_EQ(OB_SUCCESS, r.get_errno());
          ASSERT_EQ(2, r.get_affected_rows());
          ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
          ASSERT_TRUE(result_entity->is_empty());
          break;
        default:
          ASSERT_TRUE(0);
          break;
      }
    }
  }
  {
    // get and verify
    batch_operation.reset();
    entity_factory.free_and_reuse();
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->add_retrieve_property(C2));
      ASSERT_EQ(OB_SUCCESS, entity->add_retrieve_property(C3));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(BATCH_SIZE, result.count());
    ObObj value;
    ObString str;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      //fprintf(stderr, "result %ld\n", i);
      switch (i % 6) {
        case 0:  // get
        case 1:  // insert
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
          ASSERT_EQ(100+i, value.get_int());
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
          ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
          ASSERT_TRUE(str == c3_value);
          break;
        case 2:  // delete
          // entry not exist
          ASSERT_TRUE(result_entity->is_empty());
          break;
        case 3:  // update
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
          ASSERT_EQ(300+i, value.get_int());
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
          ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
          ASSERT_TRUE(str == c3_value);
          break;
        case 4:  // insert_or_update
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
          ASSERT_EQ(400+i, value.get_int());
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
          // verify the semantic of PUT
          // https://aone.alibaba-inc.com/project/81079/issue/14554345
          ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
          ASSERT_TRUE(str == c3_value);
          break;
        case 5:  // replace
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
          ASSERT_EQ(500+i, value.get_int());
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
          ASSERT_TRUE(value.is_null());
          break;
        default:
          ASSERT_TRUE(0);
          break;
      }
    }
  }
}

// the table should be single partition for this case:
// create table if not exists batch_operation_with_same_keys_test (C1 bigint primary key, C2 bigint, C3 varchar(100))
TEST_F(TestBatchExecute, batch_operation_with_same_keys)
{
  OB_LOG(INFO, "begin batch_operation_with_duplicated_keys");
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("batch_operation_with_same_keys_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  // start case
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i*2 % 10);
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //fprintf(stderr, "put key=%ld value=%ld i=%ld\n", (i*2%10), value.get_int(), i);
    ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  for (int64_t i = 0; i < BATCH_SIZE; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  // get and verify
  const ObITableEntity *result_entity = NULL;
  {
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(i*2%10);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      ObObj value;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      //fprintf(stderr, "get C2 value=%ld i=%ld key=%ld\n", value.get_int(), i, i*2%10);
      ASSERT_EQ(195+i%5, value.get_int());
    }
  }
  {
    // all the same key
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      key.set_int(0);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      ObObj value;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      //fprintf(stderr, "get C2 value=%ld i=%ld\n", value.get_int(), i);
      ASSERT_EQ(195, value.get_int());
    }
  }
  {
    // pattern: empty empty exist exist empty empty exist exist ...
    batch_operation.reset();
    entity_factory.free_and_reuse();
    ObObj null_obj;
    int64_t key1_not_exist = 1;
    int64_t key2_not_exist = 3;
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ObObj key;
      if (i < BATCH_SIZE/4) {
        key.set_int(key1_not_exist);
      } else if (i < BATCH_SIZE/4*2) {
        key.set_int(0);
      } else if (i < BATCH_SIZE/4*3) {
        key.set_int(key2_not_exist);
      } else {
        key.set_int(2);
      }
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    }
    ASSERT_TRUE(batch_operation.is_readonly());
    ASSERT_TRUE(batch_operation.is_same_type());
    ASSERT_TRUE(batch_operation.is_same_properties_names());
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    ASSERT_EQ(BATCH_SIZE, result.count());
    for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(ObTableOperationType::GET, r.type());
      ASSERT_EQ(0, r.get_affected_rows());
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      ASSERT_EQ(0, result_entity->get_rowkey_size());
      if (i < BATCH_SIZE/4) {
        ASSERT_TRUE(result_entity->is_empty());
      } else if (i < BATCH_SIZE/4*2) {
        ObObj value;
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
        //fprintf(stderr, "get C2 value=%ld i=%ld\n", value.get_int(), i);
        ASSERT_EQ(195, value.get_int());
      } else if (i < BATCH_SIZE/4*3) {
        ASSERT_TRUE(result_entity->is_empty());
      } else {
        ObObj value;
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
        //fprintf(stderr, "get C2 value=%ld i=%ld\n", value.get_int(), i);
        ASSERT_EQ(196, value.get_int());
      }
    }  // end for
  }
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

// create table type_check_test (pk1 bigint, pk2 varchar(10), ctinyint tinyint, csmallint smallint, cmediumint mediumint, cint int, cbigint bigint, utinyint tinyint unsigned, usmallint smallint unsigned, uint int unsigned, ubigint bigint unsigned, cfloat float, cdouble double, ufloat float unsigned, udouble double unsigned, cnumber decimal(10, 2), unumber decimal(10,2) unsigned, cvarchar varchar(10), cchar char(10), cbinary binary(10), cvarbinary varbinary(10), ctimestamp timestamp, cdatetime datetime, cyear year, cdate date, ctime time, ctext text, cblob blob, cbit bit(64), cnotnull bigint not null, primary key(pk1, pk2));
ObString CTINYINT = ObString::make_string("ctinyint");
ObString CNOTNULL = ObString::make_string("cnotnull");
TEST_F(TestBatchExecute, column_type_check)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("type_check_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  // case for insert operation
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObITableEntity *entity = NULL;
  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);

  int64_t pk1_value = 139107;
  ObString pk2_value = ObString::make_string("helloss");
  ObTableOperationResult r;

  ObObj pk1, pk2, value;
  {
    // case: insert + rowkey + null
    entity->reset();
    pk1.set_null();
    pk2.set_varchar(pk2_value);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_BAD_NULL_ERROR, r.get_errno());
  }
  {
    // case: insert + rowkey + collation
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_ERR_COLLATION_MISMATCH, r.get_errno());
  }
  {
    // case: insert + rowkey + int
    entity->reset();
    pk1.set_int32(111);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_OBJ_TYPE_ERROR, r.get_errno());
  }
  {
    // case: insert + rowkey + too long
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(ObString::make_string("a very loooooooooooooooooooooooooog string"));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_ERR_DATA_TOO_LONG, r.get_errno());
  }
  {
    // case: insert + not null
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_null();
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CNOTNULL, value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_BAD_NULL_ERROR, r.get_errno());
  }
  {
    // case: insert + ufloat out of range
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_ufloat(-1.0);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("ufloat"), value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_DATA_OUT_OF_RANGE, r.get_errno());
  }
  {
    // case: insert + mediumint out of range
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_mediumint(INT32_MAX);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cmediumint"), value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_DATA_OUT_OF_RANGE, r.get_errno());
  }
  {
    // case: insert succ
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));

    value.set_tinyint(-8);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("ctinyint"), value));
    value.set_smallint(-88);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("csmallint"), value));
    value.set_mediumint(-888);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cmediumint"), value));
    value.set_int32(-8888);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cint"), value));
    value.set_int(-88888);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cbigint"), value));

    value.set_utinyint(8);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("utinyint"), value));
    value.set_usmallint(88);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("usmallint"), value));
    value.set_umediumint(888);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("umediumint"), value));
    value.set_uint32(8888);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("uint"), value));
    value.set_uint64(88888);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("ubigint"), value));

    value.set_ufloat(1.0);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("ufloat"), value));
    value.set_float(-1.0);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cfloat"), value));
    value.set_udouble(1.0);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("udouble"), value));
    value.set_double(-1.0);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cdouble"), value));

    value.set_char(pk2_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cbinary"), value));
    value.set_varbinary(pk2_value);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cvarbinary"), value));
    value.set_char(pk2_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cchar"), value));
    value.set_varchar(pk2_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cvarchar"), value));

    value.set_string(ObTextType, pk2_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("ctext"), value));
    value.set_string(ObTextType, pk2_value);
    value.set_collation_type(CS_TYPE_BINARY);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cblob"), value));

    int64_t now = ObTimeUtility::current_time();
    value.set_timestamp(now);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("ctimestamp"), value));
    value.set_datetime(now);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cdatetime"), value));
    value.set_year(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cyear"), value));
    value.set_date(30);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cdate"), value));
    value.set_time(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("ctime"), value));

    value.set_bit(8);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cbit"), value));
    // cnotnull not specified and inserted as default value
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
  }

  ////////////////////////////////////////////////////////////////
  // other type of operations
  ////////////////////////////////////////////////////////////////
  {
    // case: get + rowkey + collation
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_BINARY);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_ERR_COLLATION_MISMATCH, r.get_errno());
  }
  {
    // case: replace + rowkey + collation
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_BINARY);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::replace(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_ERR_COLLATION_MISMATCH, r.get_errno());
  }
  {
    // case: insert_or_update + rowkey + collation
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_BINARY);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::insert_or_update(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_ERR_COLLATION_MISMATCH, r.get_errno());
  }
  {
    // case: delete + rowkey + collation
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_BINARY);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::del(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_ERR_COLLATION_MISMATCH, r.get_errno());
  }
  {
    // case: update + rowkey + collation
    entity->reset();
    pk1.set_int(pk1_value);
    pk2.set_varchar(pk2_value);
    pk2.set_collation_type(CS_TYPE_BINARY);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
    value.set_int(100);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(CTINYINT, value));
    ObTableOperation table_operation = ObTableOperation::update(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_ERR_COLLATION_MISMATCH, r.get_errno());
  }
  {
    // case: insert_or_update varbinary -> blob
    // TODO (luohongdi.lhd): Fix it when lob ready
//    entity->reset();
//    pk1.set_int(pk1_value);
//    pk2.set_varchar(pk2_value);
//    pk2.set_collation_type(CS_TYPE_UTF8MB4_BIN);
//
//    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
//    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk2));
//    value.set_string(ObVarcharType, ObString::make_string("cblob value"));
//    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
//    ASSERT_EQ(OB_SUCCESS, entity->set_property(ObString::make_string("cblob"), value));
//    ObTableOperation table_operation = ObTableOperation::insert_or_update(*entity);
//    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
//    ASSERT_EQ(OB_SUCCESS, r.get_errno());
  }
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

TEST_F(TestBatchExecute, column_default_value)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("column_default_value"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  // case for insert operation
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObITableEntity *entity = NULL;
  ObTableOperationResult r;
  {
    // case: insert + rowkey
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);

    int64_t pk1_value = 139107;
    ObObj pk1;
    pk1.set_int(pk1_value);
    ObObj value;
    value.set_int(pk1_value);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(pk1));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    ObTableOperation table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
  }
  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

TEST_F(TestBatchExecute, serialize_table_query)
{
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));

  ObObj pk_objs_start[2];
  pk_objs_start[0].set_int(0);
  pk_objs_start[1].set_min_value();
  ObObj pk_objs_end[2];
  pk_objs_end[0].set_int(0);
  pk_objs_end[1].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 2);
  range.end_key_.assign(pk_objs_end, 2);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
  int64_t serialize_len = query.get_serialize_size();
  fprintf(stderr, "serialize_size=%ld\n", serialize_len);
  char buf[1024];
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, query.serialize(buf, 1024, pos));
  ASSERT_EQ(pos, serialize_len);

  ObTableQuery query2;
  ObArenaAllocator alloc;
  query2.set_deserialize_allocator(&alloc);
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, query2.deserialize(buf, serialize_len, pos));
  const ObIArray<ObString> &select_columns = query2.get_select_columns();
  const ObIArray<ObNewRange> &scan_ranges = query2.get_scan_ranges();
  ASSERT_EQ(3, select_columns.count());
  ASSERT_EQ(1, scan_ranges.count());
}

TEST_F(TestBatchExecute, serialize_query_result)
{
  ObTableQueryResult query_result;
  ObObj objs[3];
  objs[0].set_int(123);
  objs[1].set_null();
  objs[2].set_varchar(ObString::make_string("serialize_query_result"));
  ObNewRow row;
  row.assign(objs, 3);
  ASSERT_EQ(OB_SUCCESS, query_result.add_property_name(C1));
  ASSERT_EQ(OB_SUCCESS, query_result.add_property_name(C2));
  ASSERT_EQ(OB_SUCCESS, query_result.add_property_name(C3));
  for (int64_t i = 0; i < 1024; ++i) {
    ASSERT_EQ(OB_SUCCESS, query_result.add_row(row));
  }
  ASSERT_EQ(1024, query_result.get_row_count());
  ASSERT_EQ(3, query_result.get_property_count());
  // serialize
  char *buf = static_cast<char*>(ob_malloc(OB_MALLOC_BIG_BLOCK_SIZE, ObModIds::TEST));
  ASSERT_TRUE(nullptr != buf);
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, query_result.serialize(buf, OB_MALLOC_BIG_BLOCK_SIZE, pos));
  ASSERT_EQ(pos, query_result.get_serialize_size());
  fprintf(stderr, "serialize_size=%ld\n", pos);
  // deserialize & check
  ObTableQueryResult query_result2;
  int64_t data_len = pos;
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, query_result2.deserialize(buf, data_len, pos));
  ASSERT_EQ(1024, query_result2.get_row_count());
  ASSERT_EQ(3, query_result2.get_property_count());
  const ObITableEntity *entity = NULL;
  for (int64_t i = 0; i < 1024; ++i) {
    ASSERT_EQ(OB_SUCCESS, query_result2.get_next_entity(entity));
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(3, entity->get_properties_count());
    ObObj value;
    ASSERT_EQ(OB_SUCCESS, entity->get_property(C1, value));
    ASSERT_EQ(123, value.get_int());
    ASSERT_EQ(OB_SUCCESS, entity->get_property(C2, value));
    ASSERT_TRUE(value.is_null());
    ASSERT_EQ(OB_SUCCESS, entity->get_property(C3, value));
    ObString str;
    ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    ASSERT_TRUE(str == ObString::make_string("serialize_query_result"));
  }
  ASSERT_EQ(OB_ITER_END, query_result2.get_next_entity(entity));
  // cleanup
  if (NULL != buf) {
    ob_free(buf);
    buf = NULL;
  }
}


// create table if not exists partial_update_test (C1 bigint primary key, C2 bigint, C3 varchar(100) not null);
TEST_F(TestBatchExecute, partial_update)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("partial_update_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableOperationResult r;
  ObITableEntity *entity = NULL;
  const ObITableEntity *result_entity = NULL;
  ObTableOperation table_operation;

  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  int64_t key_key = 139107;
  ObObj key;
  key.set_int(key_key);
  int64_t value_value = 33521;
  ObObj value;
  value.set_int(value_value);

  //prepare data
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  ObString c3_value = ObString::make_string("hello world");
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::insert(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  entity->reset();
  key.set_int(key_key + 1);
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  value.set_int(1235);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::insert(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));


  //single update
  entity->reset();
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  value.set_int(1245);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  table_operation = ObTableOperation::update(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(OB_SUCCESS, r.get_errno());
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  //insert or update
  entity->reset();
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  value.set_int(5432);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::insert_or_update(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(OB_SUCCESS, r.get_errno());
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  //batch update
  const int64_t batch_size = 2;
  ObTableBatchOperation batch_operation;
  for (int64_t i = 0; i < batch_size; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    key.set_int(key_key + i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    value.set_int(i);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.update(*entity));
  }

  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(batch_size, result.count());
  for (int64_t i = 0; i < batch_size; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  } // end for


  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}



// create table if not exists append_lob_test (C1 bigint primary key, C2 bigint, C3 mediumtext not null);
//TEST_F(TestBatchExecute, append_lob)
//{
  //// setup
  //ObTable *the_table = NULL;
  //int ret = service_client_->alloc_table(ObString::make_string("append_lob_test"), the_table);
  //ASSERT_EQ(OB_SUCCESS, ret);
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObTableOperationResult r;
  //ObITableEntity *entity = NULL;
  //const ObITableEntity *result_entity = NULL;
  //ObTableOperation table_operation;

  //entity = entity_factory.alloc();
  //ASSERT_TRUE(NULL != entity);
  //int64_t key_key = 139107;
  //ObObj key;
  //key.set_int(key_key);
  //int64_t value_value = 33521;
  //ObObj value;
  //value.set_int(value_value);

  ////prepare data
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  //ObString c3_value = ObString::make_string("hello world");
  //value.set_varchar(c3_value);
  //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  //table_operation = ObTableOperation::insert(*entity);
  //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  //ASSERT_EQ(1, r.get_affected_rows());
  //ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  //entity->reset();
  //key.set_int(key_key + 1);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //value.set_int(1235);
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  //value.set_varchar(c3_value);
  //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  //table_operation = ObTableOperation::insert(*entity);
  //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  //ASSERT_EQ(1, r.get_affected_rows());
  //ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  ////append small
  //{
    //entity->reset();
    //key.set_int(key_key);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //table_operation = ObTableOperation::append(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(true);
    //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::APPEND, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(NULL != result_entity);
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(1, result_entity->get_rowkey_size());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_rowkey_value(0, value));
    //ASSERT_EQ(key_key, value.get_int());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello worldhello world");
    //ASSERT_TRUE(str == c3_new_value);
    //ASSERT_EQ(CS_TYPE_UTF8MB4_GENERAL_CI, value.get_collation_type());
  //}

  ////append big
  //{
    //entity->reset();
    //key.set_int(key_key);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //const int32_t big_value_len = 3 * 1024 * 1024;
    //char *big_value = (char*) malloc(big_value_len);
    //ASSERT_TRUE(NULL != big_value);
    //memset(big_value, 'A', big_value_len);
    //ObString c3_big_value(big_value_len, big_value);

    //value.set_varchar(c3_big_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //table_operation = ObTableOperation::append(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(true);
    //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::APPEND, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(1, result_entity->get_rowkey_size());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_rowkey_value(0, value));
    //ASSERT_EQ(key_key, value.get_int());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ASSERT_EQ(CS_TYPE_UTF8MB4_GENERAL_CI, value.get_collation_type());

    //if (NULL != big_value) {
      //free(big_value);
      //big_value = NULL;
    //}
  //}

  //// teardown
  //service_client_->free_table(the_table);
  //the_table = NULL;
//}


// create table if not exists generate_col_test (C1 bigint primary key, C2 bigint, C3 varchar(100), C3_PREFIX varchar(10) GENERATED ALWAYS AS (substr(C3,1,2)));
//TEST_F(TestBatchExecute, generate_col_test)
//{
  //// setup
  //ObTable *the_table = NULL;
  //int ret = service_client_->alloc_table(ObString::make_string("generate_col_test"), the_table);
  //ASSERT_EQ(OB_SUCCESS, ret);
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObTableOperationResult r;
  //ObITableEntity *entity = NULL;
  //const ObITableEntity *result_entity = NULL;
  //ObTableOperation table_operation;
  //ObString C3_PREFIX = ObString::make_string("C3_PREFIX");

  //entity = entity_factory.alloc();
  //ASSERT_TRUE(NULL != entity);
  //int64_t key_key = 139107;
  //ObObj key;
  //key.set_int(key_key);
  //int64_t value_value = 33521;
  //ObObj value;
  //value.set_int(value_value);

  ////prepare data insert
  //ObString c3_value = ObString::make_string("hello world");
  //entity->reset();
  //key.set_int(key_key);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //value.set_int(value_value);
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  //value.set_varchar(c3_value);
  //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  //table_operation = ObTableOperation::insert(*entity);
  //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  //ASSERT_EQ(1, r.get_affected_rows());
  //ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  ////get and check
  //ObObj null_obj;
  //entity->reset();
  //key.set_int(key_key);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3_PREFIX, null_obj));
  //table_operation = ObTableOperation::retrieve(*entity);
  //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //ASSERT_EQ(0, r.get_affected_rows());
  //ASSERT_EQ(ObTableOperationType::GET, r.type());
  //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  //ASSERT_EQ(0, result_entity->get_rowkey_size());
  //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3_PREFIX, value));
  //ObString str;
  //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
  //ObString c3_new_value = ObString::make_string("he");
  //ASSERT_TRUE(str == c3_new_value);

  ////update and check
  //c3_value = ObString::make_string("test hello");
  //entity->reset();
  //key.set_int(key_key);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //value.set_varchar(c3_value);
  //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  //table_operation = ObTableOperation::update(*entity);
  //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  //ASSERT_EQ(1, r.get_affected_rows());
  //ASSERT_EQ(ObTableOperationType::UPDATE, r.type());
  //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  //entity->reset();
  //key.set_int(key_key);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3_PREFIX, null_obj));
  //table_operation = ObTableOperation::retrieve(*entity);
  //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //ASSERT_EQ(0, r.get_affected_rows());
  //ASSERT_EQ(ObTableOperationType::GET, r.type());
  //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  //ASSERT_EQ(0, result_entity->get_rowkey_size());
  //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3_PREFIX, value));
  //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
  //c3_new_value = ObString::make_string("te");
  //ASSERT_TRUE(str == c3_new_value);


  //// teardown
  //service_client_->free_table(the_table);
  //the_table = NULL;
//}



// create table if not exists large_scan_test (C1 bigint primary key, C2 bigint, C3 varchar(100));
TEST_F(TestBatchExecute, large_scan)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("large_scan_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableOperationResult r;
  ObITableEntity *entity = NULL;
  const ObITableEntity *result_entity = NULL;
  ObTableOperation table_operation;

  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  int64_t key_key = 139107;
  ObObj key;
  key.set_int(key_key);
  int64_t value_value = 33521;
  ObObj value;
  value.set_int(value_value);

  //prepare data
  const int64_t large_batch_size = 10000;
  ObString c3_value = ObString::make_string("hello world");
  for (int64_t i = 0; i < large_batch_size; ++i) {
    entity->reset();
    key.set_int(key_key + i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    value.set_int(value_value);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    value.set_varchar(c3_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  }
  entity->reset();

  //scan
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
  ObObj pk_objs_start;
  pk_objs_start.set_int(0);
  ObObj pk_objs_end;
  pk_objs_end.set_max_value();
  ObNewRange range;
  range.start_key_.assign(&pk_objs_start, 1);
  range.end_key_.assign(&pk_objs_end, 1);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
  query.set_scan_index(ObString::make_string("primary"));
  query.set_scan_order(ObQueryFlag::Forward);

  ObTableEntityIterator *iter = nullptr;
  ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
  int64_t result_cnt = 0;
  while (OB_SUCC(iter->get_next_entity(result_entity))) {
    result_cnt++;
  }
  ASSERT_EQ(OB_ITER_END, ret);
  ASSERT_EQ(result_cnt, large_batch_size);

  //reverse scan
  query.set_scan_order(ObQueryFlag::Reverse);
  ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
  result_cnt = 0;
  while (OB_SUCC(iter->get_next_entity(result_entity))) {
    result_cnt++;
  }
  ASSERT_EQ(OB_ITER_END, ret);
  ASSERT_EQ(result_cnt, large_batch_size);

  //large batch append
  {
    int64_t append_batch_size = 1000;
    ObTableBatchOperation batch_operation;
    for (int64_t i = 0; i < append_batch_size; ++i) {
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      key.set_int(key_key + i);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      value.set_varchar(c3_value);
      value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
      ASSERT_EQ(OB_SUCCESS, batch_operation.append(*entity));
    }

    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
    OB_LOG(INFO, "batch execute result", K(result));
    ASSERT_EQ(append_batch_size, result.count());
    for (int64_t i = 0; i < append_batch_size; ++i)
    {
      const ObTableOperationResult &r = result.at(i);
      ASSERT_EQ(OB_SUCCESS, r.get_errno());
      ASSERT_EQ(1, r.get_affected_rows());
      ASSERT_EQ(ObTableOperationType::APPEND, r.type());
      const ObITableEntity *result_entity = NULL;
      ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    } // end for
  }

  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}


// create table if not exists uniq_replace_test (C1 bigint primary key, C2 bigint, C3 varchar(100), unique key C2_UNIQ(C2));
TEST_F(TestBatchExecute, uniq_replace)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("uniq_replace_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableOperationResult r;
  ObITableEntity *entity = NULL;
  const ObITableEntity *result_entity = NULL;
  ObTableOperation table_operation;

  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  int64_t key_key = 139107;
  ObObj key;
  key.set_int(key_key);
  int64_t value_value = 33521;
  ObObj value;
  value.set_int(value_value);

  //prepare data
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  ObString c3_value = ObString::make_string("hello world");
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::insert(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  entity->reset();
  key.set_int(key_key + 1);
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  value.set_int(1235);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::insert(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::INSERT, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));


  //replace
  entity->reset();
  key.set_int(key_key + 2);
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  value.set_int(1236);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  c3_value = ObString::make_string("hello china");
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::replace(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(OB_SUCCESS, r.get_errno());
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  //replace uniq
  entity->reset();
  key.set_int(key_key + 3);
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  value.set_int(1236);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  c3_value = ObString::make_string("hello everyone");
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::replace(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //@TODO: table_api::replace need to return affected_rows=2 here,
  //but table api not maintain local index here, so affected_rows is 1
  //need to fix me
  //ASSERT_EQ(2, r.get_affected_rows());
  ASSERT_EQ(1, r.get_affected_rows());
  ASSERT_EQ(ObTableOperationType::REPLACE, r.type());
  ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));

  //insert or update, uniq key conflict, not support now
  /*
  entity->reset();
  key.set_int(key_key + 4);
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  value.set_int(1236);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
  c3_value = ObString::make_string("hello everyone");
  value.set_varchar(c3_value);
  value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
  table_operation = ObTableOperation::insert_or_update(*entity);
  ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
  ASSERT_NE(OB_SUCCESS, r.get_errno());
  ASSERT_EQ(ObTableOperationType::INSERT_OR_UPDATE, r.type());
  */

  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}



// create table if not exists execute_query_test (PK1 bigint, PK2 bigint, C1 bigint, C2 varchar(100), C3 bigint, PRIMARY KEY(PK1, PK2), INDEX idx1(C1, C2));
ObString PK1 = ObString::make_string("PK1");
ObString PK2 = ObString::make_string("PK2");
TEST_F(TestBatchExecute, query_pk_prefix)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("execute_query_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i%5);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    key.set_int(i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C1, value));
    value.set_varchar(ObString::make_string("c2_value"));
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  for (int64_t i = 0; i < BATCH_SIZE; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  // cases
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(PK2));
  ObObj pk_objs_start[2];
  pk_objs_start[0].set_int(0);
  pk_objs_start[1].set_min_value();
  ObObj pk_objs_end[2];
  pk_objs_end[0].set_int(0);
  pk_objs_end[1].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 2);
  range.end_key_.assign(pk_objs_end, 2);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
  query.set_offset(5);
  query.set_limit(10);
  query.set_scan_index(ObString::make_string("primary"));
  ObQueryFlag::ScanOrder scan_orders[2] = { ObQueryFlag::Forward, ObQueryFlag::Reverse };
  for (int k = 0; k < 2; ++k)
  {
    // two scan order
    query.set_scan_order(scan_orders[k]);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    for (int64_t i = 5; i < 15; ++i)
    {
      ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
      ASSERT_EQ(4, result_entity->get_properties_count());
      ObObj value;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C1, value));
      if (0 == k) {
        ASSERT_EQ(100+i*5, value.get_int());
      } else {
        ASSERT_EQ(100+(19-i)*5, value.get_int());
      }
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      ObString str;
      ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
      ASSERT_TRUE(str == ObString::make_string("c2_value"));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
      ASSERT_TRUE(value.is_null());
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(PK2, value));
      if (0 == k) {
        ASSERT_EQ(i*5, value.get_int());
      } else {
        ASSERT_EQ((19-i)*5, value.get_int());
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  } // end for
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

TEST_F(TestBatchExecute, compare_cell)
{
  ObObj row1_objs[4];
  ObNewRow row1(row1_objs, 4);
  ObHTableCellEntity cell1(&row1);
  ObObj row2_objs[4];
  ObNewRow row2(row2_objs, 4);
  ObHTableCellEntity cell2(&row2);
  oceanbase::common::ObQueryFlag::ScanOrder scan_order = ObQueryFlag::Forward;
  // case 1
  row1_objs[ObHTableConstants::COL_IDX_K].set_varchar(ObString::make_string("abc"));
  row1_objs[ObHTableConstants::COL_IDX_Q].set_varchar(ObString::make_string(""));
  row1_objs[ObHTableConstants::COL_IDX_T].set_int(-2);
  row1_objs[ObHTableConstants::COL_IDX_V].set_varchar(ObString::make_string("value1"));
  row2_objs[ObHTableConstants::COL_IDX_K].set_varchar(ObString::make_string("abc"));
  row2_objs[ObHTableConstants::COL_IDX_Q].set_varchar(ObString::make_string(""));
  row2_objs[ObHTableConstants::COL_IDX_T].set_int(-2);
  row2_objs[ObHTableConstants::COL_IDX_V].set_varchar(ObString::make_string("value2"));
  ASSERT_EQ(0, ObHTableUtils::compare_cell(cell1, cell2, scan_order));
  // case 1: compare timestamp
  row2_objs[ObHTableConstants::COL_IDX_T].set_int(-3);
  ASSERT_EQ(1, ObHTableUtils::compare_cell(cell1, cell2, scan_order));
  row1_objs[ObHTableConstants::COL_IDX_T].set_int(-4);
  ASSERT_EQ(-1, ObHTableUtils::compare_cell(cell1, cell2, scan_order));
  row1_objs[ObHTableConstants::COL_IDX_Q].set_varchar(ObString::make_string("c1"));
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell1, cell2, scan_order) > 0);
  // case 2: last on row
  ObHTableLastOnRowCell last_on_row(ObString::make_string("abc"));
  ASSERT_EQ(-1, ObHTableUtils::compare_cell(cell1, last_on_row, scan_order));
  ASSERT_EQ(-1, ObHTableUtils::compare_cell(cell2, last_on_row, scan_order));
  ASSERT_EQ(1, ObHTableUtils::compare_cell(last_on_row, cell1, scan_order));
  ASSERT_EQ(1, ObHTableUtils::compare_cell(last_on_row, cell2, scan_order));
  // case 3: last on column
  ObHTableLastOnRowColCell last_on_col(ObString::make_string("abc"), ObString::make_string(""));
  ASSERT_EQ(-1, ObHTableUtils::compare_cell(cell2, last_on_col, scan_order));
  ASSERT_EQ(1, ObHTableUtils::compare_cell(last_on_col, cell2, scan_order));
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell1, last_on_col, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_col, cell1, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_col, last_on_row, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_row, last_on_col, scan_order) > 0);
  ObHTableLastOnRowColCell last_on_col2(ObString::make_string("abc"), ObString::make_string(""));
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_col, last_on_col2, scan_order) == 0);
  // case 4: first on row
  ObHTableFirstOnRowCell first_on_row(ObString::make_string("abc"));
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell1, first_on_row, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell2, first_on_row, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_row, cell1, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_row, cell2, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_row, last_on_col, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_col, first_on_row, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_row, last_on_row, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_row, first_on_row, scan_order) > 0);
  // case 5: first on column
  ObHTableFirstOnRowColCell first_on_col(ObString::make_string("abc"), ObString::make_string(""));
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell2, first_on_col, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_col, cell2, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell1, first_on_col, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_col, cell1, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_col, first_on_row, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_row, first_on_col, scan_order) < 0);
  ObHTableFirstOnRowColCell first_on_col2(ObString::make_string("abc"), ObString::make_string(""));
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_col, first_on_col2, scan_order) == 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_col, last_on_col, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_col, first_on_col, scan_order) > 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(first_on_col, last_on_row, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(last_on_row, first_on_col, scan_order) > 0);
  // case 6: compare rowkey
  row2_objs[ObHTableConstants::COL_IDX_K].set_varchar(ObString::make_string("abc1"));
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell1, cell2, scan_order) < 0);
  ASSERT_TRUE(ObHTableUtils::compare_cell(cell2, cell1, scan_order) > 0);
}

ObString K = ObString::make_string("K");
ObString Q = ObString::make_string("Q");
ObString T = ObString::make_string("T");
ObString V = ObString::make_string("V");
// create table htable1_cf1 (K varbinary(1024), Q varbinary(256), T bigint, V varbinary(1024), primary key(K, Q, T)) partition by key(K) partitions 16;
TEST_F(TestBatchExecute, htable_scan_basic)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  DefaultBuf *rows = new (std::nothrow) DefaultBuf[BATCH_SIZE];
  ASSERT_TRUE(NULL != rows);
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  ObObj key1, key2, key3;
  ObObj value;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    sprintf(rows[i], "row%ld", i);
    key1.set_varbinary(ObString::make_string(rows[i]));
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      key2.set_varbinary(ObString::make_string(qualifier[j]));
      for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
      {
        key3.set_int(k);
        entity = entity_factory.alloc();
        ASSERT_TRUE(NULL != entity);
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
        value.set_varbinary(ObString::make_string("value_string"));
        ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
        ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
      } // end for
    }   // end for
  }     // end for

  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT, result.count());
  for (int64_t i = 0; i < BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  // htable filter cases
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
  ObObj pk_objs_start[3];
  pk_objs_start[0].set_varbinary(ObString::make_string("row50"));
  pk_objs_start[1].set_min_value();
  pk_objs_start[2].set_min_value();
  ObObj pk_objs_end[3];
  pk_objs_end[0].set_varbinary(ObString::make_string("row59"));
  pk_objs_end[1].set_max_value();
  pk_objs_end[2].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 3);
  range.end_key_.assign(pk_objs_end, 3);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));

  ObHTableFilter &htable_filter = query.htable_filter();
  int cqids[6] = {0, 3, 1, 4, 7, 9};
  for (int i = 0; OB_SUCCESS == ret && i < 6; ++i)
  {
    sprintf(qualifier[i], "cq%d", cqids[i]);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier[i])));
  } // end for
  htable_filter.set_valid(true);
  // Case 0
  fprintf(stderr, "Case 0: without max version or time range\n");
  {
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    int64_t timestamps[] = {9};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // Case 1
  fprintf(stderr, "Case 1: with max version and time range\n");
  {
    htable_filter.set_max_versions(2);
    htable_filter.set_time_range(3, 8);
    htable_filter.set_row_offset_per_column_family(2);
    htable_filter.set_max_results_per_column_family(8);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  fprintf(stderr, "Case 2: set max version only\n");
  {
    htable_filter.set_max_versions(5);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, ObHTableConstants::INITIAL_MAX_STAMP);
    htable_filter.set_row_offset_per_column_family(0);
    htable_filter.set_max_results_per_column_family(-1);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    int64_t timestamps[] = {9, 8, 7, 6, 5};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  fprintf(stderr, "Case 3: multiple result packet\n");
  {
    htable_filter.set_max_versions(1);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, ObHTableConstants::INITIAL_MAX_STAMP);
    htable_filter.set_row_offset_per_column_family(0);
    htable_filter.set_max_results_per_column_family(-1);
    query.set_batch(3);
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    int64_t timestamps[] = {9};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  fprintf(stderr, "Case 5: Wildcard column tracker\n");
  {
    htable_filter.set_max_versions(5);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, 8);
    htable_filter.set_row_offset_per_column_family(5);
    htable_filter.set_max_results_per_column_family(-1);
    htable_filter.clear_columns();
    query.set_batch(-1);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    int64_t timestamps[] = {7, 6, 5, 4, 3};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      //fprintf(stderr, "i=%ld\n", i);
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }  // end for
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
  delete [] rows;
}

TEST_F(TestBatchExecute, htable_scan_reverse)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_reverse"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  DefaultBuf *rows = new (std::nothrow) DefaultBuf[BATCH_SIZE];
  ASSERT_TRUE(NULL != rows);
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  ObObj key1, key2, key3;
  ObObj value;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    sprintf(rows[i], "row%ld", i);
    key1.set_varbinary(ObString::make_string(rows[i]));
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      key2.set_varbinary(ObString::make_string(qualifier[j]));
      for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
      {
        key3.set_int(k);
        entity = entity_factory.alloc();
        ASSERT_TRUE(NULL != entity);
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
        value.set_varbinary(ObString::make_string("value_string"));
        ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
        ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
      } // end for
    }   // end for
  }     // end for

  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT, result.count());
  for (int64_t i = 0; i < BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  // htable filter cases
  ObTableQuery query;
  query.set_scan_order(ObQueryFlag::Reverse);
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
  ObObj pk_objs_start[3];
  pk_objs_start[0].set_varbinary(ObString::make_string("row50"));
  pk_objs_start[1].set_min_value();
  pk_objs_start[2].set_min_value();
  ObObj pk_objs_end[3];
  pk_objs_end[0].set_varbinary(ObString::make_string("row59"));
  pk_objs_end[1].set_max_value();
  pk_objs_end[2].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 3);
  range.end_key_.assign(pk_objs_end, 3);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));

  ObHTableFilter &htable_filter = query.htable_filter();
  int cqids[6] = {0, 3, 1, 4, 7, 9};
  for (int i = 0; OB_SUCCESS == ret && i < 6; ++i)
  {
    sprintf(qualifier[i], "cq%d", cqids[i]);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier[i])));
  } // end for
  htable_filter.set_valid(true);
  //  Case 0
  fprintf(stderr, "Case 0: reverse scan without max version or time range\n");
  {
    ObTableEntityIterator *iter = nullptr;

    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    int64_t timestamps[] = {9};
    for (int64_t i = 0, m = 59; i < 10; ++i, --m) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", m);
      key1.set_varbinary(ObString::make_string(rows[i]));

      for (int64_t j = 0, n = 5; j < ARRAYSIZEOF(cqids_sorted); ++j,--n) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[n]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));

        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          // fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

fprintf(stderr, "Case 1: reverse scan set max version only\n");
  {
    htable_filter.set_max_versions(5);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, ObHTableConstants::INITIAL_MAX_STAMP);
    htable_filter.set_row_offset_per_column_family(0);
    htable_filter.set_max_results_per_column_family(-1);

    ObTableEntityIterator *iter = nullptr;
    query.set_scan_order(ObQueryFlag::Reverse);
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    int64_t timestamps[] = {9, 8, 7, 6, 5};
    for (int64_t i = 0, m = 59; i < 10; ++i,--m) {
    // 10 rowkeys
    sprintf(rows[i], "row%ld", m);
    key1.set_varbinary(ObString::make_string(rows[i]));
    for (int64_t j = 0, n = 5; j < ARRAYSIZEOF(cqids_sorted); ++j, --n) {
      // 4 qualifier
      sprintf(qualifier2[j], "cq%d", cqids_sorted[n]);
      key2.set_varbinary(ObString::make_string(qualifier2[j]));
      for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
      {
        key3.set_int(timestamps[k]);
        ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
        ObObj rk, cq, ts, val;
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
        // fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
        ASSERT_EQ(key1, rk);
        ASSERT_EQ(key2, cq);
        ASSERT_EQ(key3, ts);
      } // end for
    }
  }
  ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  fprintf(stderr, "Case 2: multiple result packet\n");
  {
    htable_filter.set_max_versions(1);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, ObHTableConstants::INITIAL_MAX_STAMP);
    htable_filter.set_row_offset_per_column_family(0);
    htable_filter.set_max_results_per_column_family(-1);
    query.set_batch(3);
    ObTableEntityIterator *iter = nullptr;
    query.set_scan_order(ObQueryFlag::Reverse);
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    int64_t timestamps[] = {9};
    for (int64_t i = 0, m = 59; i < 10; ++i, --m) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", m);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0, n = 5; j < ARRAYSIZEOF(cqids_sorted); ++j, --n) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[n]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          // fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

 fprintf(stderr, "Case 3: Wildcard column tracker\n");
  {
    query.set_scan_order(ObQueryFlag::Reverse);
    htable_filter.set_max_versions(5);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, 8);
    htable_filter.set_row_offset_per_column_family(0);  //in every row ,the first qualifier's five cells will be skipped
    htable_filter.set_max_results_per_column_family(-1);
    htable_filter.clear_columns();
    query.set_batch(-1);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    int64_t timestamps[] = {7, 6, 5, 4, 3};
    for (int64_t i = 0 , m = 59; i < 10; ++i, --m) {
      // 10 rowkeys
      //fprintf(stderr, "i=%ld\n", i);
      sprintf(rows[i], "row%ld", m);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0 , n = 9; j < ARRAYSIZEOF(cqids_sorted); ++j, --n) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[n]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          // fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }  // end for
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
}

TEST_F(TestBatchExecute, htable_scan_with_filter)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_filter"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  DefaultBuf *rows = new (std::nothrow) DefaultBuf[BATCH_SIZE];
  ASSERT_TRUE(NULL != rows);
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  ObObj key1, key2, key3;
  ObObj value;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    sprintf(rows[i], "row%ld", i);
    key1.set_varbinary(ObString::make_string(rows[i]));
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      key2.set_varbinary(ObString::make_string(qualifier[j]));
      for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
      {
        key3.set_int(k);
        entity = entity_factory.alloc();
        ASSERT_TRUE(NULL != entity);
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
        switch (i % 4) {
          case 0:  // row52
            value.set_varbinary(ObString::make_string("xx_string1"));
            break;
          case 1:  // row53
            value.set_varbinary(ObString::make_string("aa_string1"));
            break;
          case 2:  // row50
            value.set_varbinary(ObString::make_string("xy_string2"));
            break;
          case 3:  // row51
            value.set_varbinary(ObString::make_string("ab_string2"));
            break;
          default:
            ASSERT_TRUE(0);
        }
        ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
        ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
      } // end for
    }   // end for
  }     // end for

  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT, result.count());
  for (int64_t i = 0; i < BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  // htable filter cases
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
  ObObj pk_objs_start[3];
  pk_objs_start[0].set_varbinary(ObString::make_string("row50"));
  pk_objs_start[1].set_min_value();
  pk_objs_start[2].set_min_value();
  ObObj pk_objs_end[3];
  pk_objs_end[0].set_varbinary(ObString::make_string("row59"));
  pk_objs_end[1].set_max_value();
  pk_objs_end[2].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 3);
  range.end_key_.assign(pk_objs_end, 3);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));

  ObHTableFilter &htable_filter = query.htable_filter();
  int cqids[6] = {0, 3, 1, 4, 7, 9};
  for (int i = 0; OB_SUCCESS == ret && i < 6; ++i)
  {
    sprintf(qualifier[i], "cq%d", cqids[i]);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier[i])));
  } // end for
  htable_filter.set_max_versions(2);
  htable_filter.set_time_range(3, 8);
  htable_filter.set_row_offset_per_column_family(2);
  htable_filter.set_max_results_per_column_family(8);
  htable_filter.set_valid(true);
  {
    fprintf(stderr, "case: = binary comparator\n");
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', =, 'binary:xy_string2', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    // check
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 3; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i*4+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          v.set_varbinary(ObString::make_string("xy_string2"));
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case : > binary filter
  fprintf(stderr, "case: > binary comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', >, 'binary:w', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 5; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+2*i);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("xy_string2"));
          } else {
            v.set_varbinary(ObString::make_string("xx_string1"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case: < binary filter
  fprintf(stderr, "case: < binary comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', <, 'binary:w', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 5; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i*2+1);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("ab_string2"));
          } else {
            v.set_varbinary(ObString::make_string("aa_string1"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case: >= binary filter
  fprintf(stderr, "case: >= binary comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', >=, 'binary:xx_string1', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 5; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i*2);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("xy_string2"));
          } else {
            v.set_varbinary(ObString::make_string("xx_string1"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case: <= binary filter
  fprintf(stderr, "case: <= binary comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', <=, 'binary:ab_string2', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 5; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i*2+1);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("ab_string2"));
          } else {
            v.set_varbinary(ObString::make_string("aa_string1"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case: prefix filter
  fprintf(stderr, "case: = prefix comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', =, 'binaryprefix:a', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 5; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i*2+1);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("ab_string2"));
          } else {
            v.set_varbinary(ObString::make_string("aa_string1"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case: != prefix filter
  fprintf(stderr, "case: != prefix comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', !=, 'binaryprefix:x', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 5; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i*2+1);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("ab_string2"));
          } else {
            v.set_varbinary(ObString::make_string("aa_string1"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case: = substring filter
  fprintf(stderr, "case: = substring comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', =, 'substring:string1', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 4; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i/2*4+2+(i%2));
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("xx_string1"));
          } else {
            v.set_varbinary(ObString::make_string("aa_string1"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: != substring filter
  fprintf(stderr, "case: != substring comparator\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("SingleColumnValueFilter('cf1', 'cq1', !=, 'substring:string1', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 6; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i/2*4+(i%2));
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("xy_string2"));
          } else {
            v.set_varbinary(ObString::make_string("ab_string2"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: ValueFilter
  fprintf(stderr, "case: ValueFilter\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("ValueFilter(=, 'binary:aa_string1')"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 2; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i*4+3);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          v.set_varbinary(ObString::make_string("aa_string1"));
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: QualifierFilter
  fprintf(stderr, "case: QualifierFilter\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("QualifierFilter(>=, 'binary:cq1')"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {3, 4, 7, 9};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case: RowFilter
  fprintf(stderr, "case: RowFilter\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("RowFilter(>=, 'binary:row55')"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 5; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 55+i);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: SkipFilter
  fprintf(stderr, "case: SkipFilter\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("Skip QualifierFilter(>=, 'binary:cq1')"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: WhileMatchFilter
  fprintf(stderr, "case: WhileMatchFilter\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("While SingleColumnValueFilter('cf1', 'cq1', !=, 'substring:string1', true, false)"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 2; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i/2*4+(i%2));
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ObObj v;
          if (i % 2 == 0) {
            v.set_varbinary(ObString::make_string("xy_string2"));
          } else {
            v.set_varbinary(ObString::make_string("ab_string2"));
          }
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
          ASSERT_EQ(val, v);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: FilterListAND
  fprintf(stderr, "case: FilterListAND\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("RowFilter(>=, 'binary:row55') AND QualifierFilter(>=, 'binary:cq1') AND SKIP ValueFilter(!=, 'binary:ab_string2')"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {3, 4, 7, 9};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 3; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 56+i);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: FilterListOR
  fprintf(stderr, "case: FilterListOR\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("RowFilter(>=, 'binary:row55') OR ValueFilter(=, 'binary:ab_string2')"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 6; ++i) {
      // 10 rowkeys
      if (i <=0) {
        sprintf(rows[i], "row51");
      } else {
        sprintf(rows[i], "row%ld", 54+i);
      }
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // case: FilterListOR
  fprintf(stderr, "case: FilterListOR 2\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("ValueFilter(=, 'binary:xx_string1') OR ValueFilter(=, 'binary:ab_string2') OR ValueFilter(=, 'binary:xy_string2')"));
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[4] = {1, 3, 4, 7};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 10; ++i) {
      if (i % 4 == 3) {
        continue;
      }
      sprintf(rows[i], "row%ld", 50+i);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 4; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 2; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  // case : ColumnCountGetFilter
  fprintf(stderr, "case: ColumnCountGetFilter\n");
  // check
  {
    htable_filter.set_filter(ObString::make_string("ColumnCountGetFilter(3)"));
    htable_filter.set_max_versions(1);
    htable_filter.set_row_offset_per_column_family(0);
    htable_filter.set_max_results_per_column_family(-1);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    htable_filter.set_max_versions(2);
    htable_filter.set_row_offset_per_column_family(2);
    htable_filter.set_max_results_per_column_family(8);

    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[3] = {0, 1, 3};
    int64_t timestamps[2] = {7, 6};
    for (int64_t i = 0; i < 1; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", 50+i);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < 3; ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 1; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
  delete [] rows;
}

TEST_F(TestBatchExecute, htable_delete)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_delete"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  DefaultBuf *rows = new (std::nothrow) DefaultBuf[BATCH_SIZE];
  ASSERT_TRUE(NULL != rows);
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  ObObj key1, key2, key3;
  ObObj value;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    sprintf(rows[i], "row%ld", i);
    key1.set_varbinary(ObString::make_string(rows[i]));
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      key2.set_varbinary(ObString::make_string(qualifier[j]));
      for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
      {
        key3.set_int(k);
        entity = entity_factory.alloc();
        ASSERT_TRUE(NULL != entity);
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
        switch (i % 4) {
          case 0:
            value.set_varbinary(ObString::make_string("string2"));
            break;
          case 1:
            value.set_varbinary(ObString::make_string("string3"));
            break;
          case 2:  // row50
            value.set_varbinary(ObString::make_string("string0"));
            break;
          case 3:
            value.set_varbinary(ObString::make_string("string1"));
            break;
          default:
            ASSERT_TRUE(0);
        }
        ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
        ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
      } // end for
    }   // end for
  }     // end for

  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT, result.count());
  for (int64_t i = 0; i < BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  ////////////////////////////////////////////////////////////////
  {
    fprintf(stderr, "case: delete by row\n");
    const char* rowkey = "row1";
    batch_operation.reset();
    sprintf(rows[0], "%s", rowkey);
    key1.set_varbinary(ObString::make_string(rows[0]));
    key2.set_null();          // delete all qualifier
    key3.set_int(-INT64_MAX);  // delete all version
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));

    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));

    // verify
    ObTableQuery query;
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
    ObObj pk_objs_start[3];
    pk_objs_start[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_start[1].set_min_value();
    pk_objs_start[2].set_min_value();
    ObObj pk_objs_end[3];
    pk_objs_end[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_end[1].set_max_value();
    pk_objs_end[2].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 3);
    range.end_key_.assign(pk_objs_end, 3);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();
    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ObHTableFilter &htable_filter = query.htable_filter();
    htable_filter.set_valid(true);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  {
    fprintf(stderr, "case: delete by qualifier: cq3, cq5\n");
    const char* rowkey = "row2";
    batch_operation.reset();
    sprintf(rows[0], "%s", rowkey);
    key1.set_varbinary(ObString::make_string(rows[0]));
    int cqids[] = {3, 5};
    for (int64_t j = 0; j < ARRAYSIZEOF(cqids); ++j) {
      sprintf(qualifier2[j], "cq%d", cqids[j]);
      key2.set_varbinary(ObString::make_string(qualifier2[j]));
      key3.set_int(-INT64_MAX);  // delete all version
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));
    }
    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));

    // verify
    ObTableQuery query;
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
    ObObj pk_objs_start[3];
    pk_objs_start[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_start[1].set_min_value();
    pk_objs_start[2].set_min_value();
    ObObj pk_objs_end[3];
    pk_objs_end[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_end[1].set_max_value();
    pk_objs_end[2].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 3);
    range.end_key_.assign(pk_objs_end, 3);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();
    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ObHTableFilter &htable_filter = query.htable_filter();
    htable_filter.set_valid(true);
    htable_filter.clear_columns();
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 2, 4, 6, 7, 8, 9};
    int64_t timestamps[] = {9};
    for (int64_t i = 0; i < 1; ++i) {
      key1.set_varbinary(ObString::make_string(rowkey));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < 1; ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  {
    fprintf(stderr, "case: delete by qualifier & version: cq3 & version5\n");
    const char* rowkey = "row3";
    const char* cq = "cq3";
    int64_t ts = 5;
    batch_operation.reset();
    sprintf(rows[0], "%s", rowkey);
    key1.set_varbinary(ObString::make_string(rows[0]));
    key2.set_varbinary(ObString::make_string(cq));
    key3.set_int(ts);  // delete the specified version
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));

    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));

    // verify
    ObTableQuery query;
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
    ObObj pk_objs_start[3];
    pk_objs_start[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_start[1].set_min_value();
    pk_objs_start[2].set_min_value();
    ObObj pk_objs_end[3];
    pk_objs_end[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_end[1].set_max_value();
    pk_objs_end[2].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 3);
    range.end_key_.assign(pk_objs_end, 3);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();
    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ObHTableFilter &htable_filter = query.htable_filter();
    htable_filter.set_valid(true);
    htable_filter.clear_columns();
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(cq)));
    htable_filter.set_max_versions(INT32_MAX);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, ObHTableConstants::INITIAL_MAX_STAMP);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int64_t timestamps[] = {9, 8, 7, 6, 4, 3, 2, 1, 0};
    for (int64_t i = 0; i < 1; ++i) {
      key1.set_varbinary(ObString::make_string(rowkey));
      for (int64_t j = 0; j < 1; ++j) {
        key2.set_varbinary(ObString::make_string(cq));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  {
    fprintf(stderr, "case: delete by qualifier & version: cq3 & INT64_MAX\n");
    const char* rowkey = "row4";
    const char* cq = "cq3";
    batch_operation.reset();
    sprintf(rows[0], "%s", rowkey);
    key1.set_varbinary(ObString::make_string(rows[0]));
    key2.set_varbinary(ObString::make_string(cq));
    key3.set_int(INT64_MAX);  // delete the latest version
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));

    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));

    // verify
    ObTableQuery query;
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
    ObObj pk_objs_start[3];
    pk_objs_start[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_start[1].set_min_value();
    pk_objs_start[2].set_min_value();
    ObObj pk_objs_end[3];
    pk_objs_end[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_end[1].set_max_value();
    pk_objs_end[2].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 3);
    range.end_key_.assign(pk_objs_end, 3);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();
    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ObHTableFilter &htable_filter = query.htable_filter();
    htable_filter.set_valid(true);
    htable_filter.clear_columns();
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(cq)));
    htable_filter.set_max_versions(INT32_MAX);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, ObHTableConstants::INITIAL_MAX_STAMP);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int64_t timestamps[] = {8, 7, 6, 5, 4, 3, 2, 1, 0};
    for (int64_t i = 0; i < 1; ++i) {
      key1.set_varbinary(ObString::make_string(rowkey));
      for (int64_t j = 0; j < 1; ++j) {
        key2.set_varbinary(ObString::make_string(cq));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));

  }
  {
    fprintf(stderr, "case: delete by qualifier & version: cq3 & version<=5\n");
    const char* rowkey = "row5";
    const char* cq = "cq3";
    int64_t ts = 5;
    batch_operation.reset();
    sprintf(rows[0], "%s", rowkey);
    key1.set_varbinary(ObString::make_string(rows[0]));
    key2.set_varbinary(ObString::make_string(cq));
    key3.set_int(-ts);  // delete the versions < 5
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));

    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));

    // verify
    ObTableQuery query;
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
    ObObj pk_objs_start[3];
    pk_objs_start[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_start[1].set_min_value();
    pk_objs_start[2].set_min_value();
    ObObj pk_objs_end[3];
    pk_objs_end[0].set_varbinary(ObString::make_string(rowkey));
    pk_objs_end[1].set_max_value();
    pk_objs_end[2].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 3);
    range.end_key_.assign(pk_objs_end, 3);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();
    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ObHTableFilter &htable_filter = query.htable_filter();
    htable_filter.set_valid(true);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(cq)));
    htable_filter.set_max_versions(INT32_MAX);
    htable_filter.set_time_range(ObHTableConstants::INITIAL_MIN_STAMP, ObHTableConstants::INITIAL_MAX_STAMP);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int64_t timestamps[] = {9, 8, 7, 6};
    for (int64_t i = 0; i < 1; ++i) {
      key1.set_varbinary(ObString::make_string(rowkey));
      for (int64_t j = 0; j < 1; ++j) {
        key2.set_varbinary(ObString::make_string(cq));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
  delete [] rows;
}

TEST_F(TestBatchExecute, htable_query_and_mutate)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_query_and_mutate"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  DefaultBuf *rows = new (std::nothrow) DefaultBuf[BATCH_SIZE];
  ASSERT_TRUE(NULL != rows);
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  ObObj key1, key2, key3;
  ObObj value;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    sprintf(rows[i], "row%ld", i);
    key1.set_varbinary(ObString::make_string(rows[i]));
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      key2.set_varbinary(ObString::make_string(qualifier[j]));
      for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
      {
        key3.set_int(k);
        entity = entity_factory.alloc();
        ASSERT_TRUE(NULL != entity);
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
        switch (i % 4) {
          case 0:
            value.set_varbinary(ObString::make_string("string2"));
            break;
          case 1:
            value.set_varbinary(ObString::make_string("string3"));
            break;
          case 2:  // row50
            value.set_varbinary(ObString::make_string("string0"));
            break;
          case 3:
            value.set_varbinary(ObString::make_string("string1"));
            break;
          default:
            ASSERT_TRUE(0);
        }
        ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
        ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
      } // end for
    }   // end for
  }     // end for

  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT, result.count());
  for (int64_t i = 0; i < BATCH_SIZE*COLUMNS_SIZE*VERSIONS_COUNT; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  // htable filter cases
  ObTableQueryAndMutate query_and_mutate;
  ObTableQuery &query = query_and_mutate.get_query();
  ObTableBatchOperation &mutations = query_and_mutate.get_mutations();
  ObObj pk_objs_start[3];
  ObObj pk_objs_end[3];
  ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, "row50"));

  ObHTableFilter &htable_filter = query.htable_filter();
  int cqids[6] = {0, 3, 1, 4, 7, 9};
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; OB_SUCCESS == ret && i < 6; ++i)
  {
    sprintf(qualifier2[i], "cq%d", cqids[i]);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier2[i])));
  } // end for
  htable_filter.set_valid(true);

  {
    fprintf(stderr, "case: simple query and put\n");
    // set mutation
    sprintf(rows[0], "row50");
    key1.set_varbinary(ObString::make_string(rows[0]));
    key3.set_int(VERSIONS_COUNT);
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      key2.set_varbinary(ObString::make_string(qualifier[j]));
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      value.set_varbinary(ObString::make_string("value_kaka"));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
      ASSERT_EQ(OB_SUCCESS, mutations.insert_or_update(*entity));
    }   // end for
    ObTableQueryAndMutateResult result;
    int64_t &affected_rows = result.affected_rows_;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query_and_mutate(query_and_mutate, result));
    fprintf(stderr, "query_and_mutate affected_rows=%ld\n", affected_rows);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    int64_t timestamps[] = {10};
    for (int64_t i = 0; i < 1; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  {
    fprintf(stderr, "case: simple query and delete columns\n");
    entity_factory.free_and_reuse();
    mutations.reset();
    // set delete mutation
    const char* rowkey = "row50";
    key1.set_varbinary(ObString::make_string(rowkey));
    int cqids_sorted[] = {0, 1, 3, 4, 7, 9};
    // delete version 10 of cq 0, 1, 3, 4, 7, 9
    for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
      sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
      key2.set_varbinary(ObString::make_string(qualifier2[j]));
      key3.set_int(INT64_MAX);  // delete latest version
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      ASSERT_EQ(OB_SUCCESS, mutations.del(*entity));
    }
    ObTableQueryAndMutateResult result;
    int64_t &affected_rows = result.affected_rows_;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query_and_mutate(query_and_mutate, result));
    fprintf(stderr, "query_and_mutate affected_rows=%ld\n", affected_rows);

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int64_t timestamps[] = {9};
    for (int64_t i = 0; i < 1; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  {
    fprintf(stderr, "case: hbase checkAndPut (value not null)\n");
    // row50 and row54 will put cq110
    // set query
    htable_filter.clear_columns();
    // select all columns
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier[j])));
    }
    htable_filter.set_filter(ObString::make_string("CheckAndMutateFilter(=, 'substring:string0', 'cf1', 'cq1', false)"));
    ObNewRange range;
    // set mutation
    for (int64_t i = 0; i < 8; ++i) {  // row50 ~ row57
      sprintf(rows[0], "row%ld", 50+i);
      pk_objs_start[0].set_varbinary(ObString::make_string(rows[0]));
      pk_objs_start[1].set_min_value();
      pk_objs_start[2].set_min_value();
      pk_objs_end[0].set_varbinary(ObString::make_string(rows[0]));
      pk_objs_end[1].set_max_value();
      pk_objs_end[2].set_max_value();
      range.start_key_.assign(pk_objs_start, 3);
      range.end_key_.assign(pk_objs_end, 3);
      range.border_flag_.set_inclusive_start();
      range.border_flag_.set_inclusive_end();
      query.clear_scan_range();
      ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));

      entity_factory.free_and_reuse();
      mutations.reset();
      key1.set_varbinary(ObString::make_string(rows[0]));
      key3.set_int(VERSIONS_COUNT);
      key2.set_varbinary(ObString::make_string("cq110"));
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      value.set_varbinary(ObString::make_string("string110"));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
      ASSERT_EQ(OB_SUCCESS, mutations.insert_or_update(*entity));
      ObTableQueryAndMutateResult result;
      int64_t &affected_rows = result.affected_rows_;
      ret = the_table->execute_query_and_mutate(query_and_mutate, result);
      if (i % 4 == 0) {
        ASSERT_EQ(OB_SUCCESS, ret);
        ASSERT_EQ(1, affected_rows);
      } else {
        ASSERT_EQ(OB_NOT_SUPPORTED, ret);
      }
      fprintf(stderr, "query_and_mutate affected_rows=%ld\n", affected_rows);
    }   // end for

    // scan row50 ~ row57
    pk_objs_start[0].set_varbinary(ObString::make_string("row50"));
    pk_objs_start[1].set_min_value();
    pk_objs_start[2].set_min_value();
    pk_objs_end[0].set_varbinary(ObString::make_string("row58"));
    pk_objs_end[1].set_min_value();
    pk_objs_end[2].set_min_value();
    range.start_key_.assign(pk_objs_start, 3);
    range.end_key_.assign(pk_objs_end, 3);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.unset_inclusive_end();
    query.clear_scan_range();
    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    htable_filter.clear_columns();
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string("cq110")));

    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {110};
    int64_t timestamps[] = {VERSIONS_COUNT};
    for (int64_t i = 0; i < 2; ++i) {
      // 2 rows
      if (i == 0) {
        sprintf(rows[0], "row50");
      } else {
        sprintf(rows[0], "row54");
      }
      key1.set_varbinary(ObString::make_string(rows[0]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  {
    fprintf(stderr, "case: hbase checkAndDelete (value is null)\n");
    // assert: columns except row50 and row54 will be deleted
    // set query
    htable_filter.set_max_versions(INT32_MAX);
    htable_filter.clear_columns();
    htable_filter.set_filter(ObString::make_string("CheckAndMutateFilter(=, 'substring:string0', 'cf1', 'cq110', true)"));
    ObNewRange range;
    // set mutation
    for (int64_t i = 0; i < 8; ++i) {  // row50 ~ row57
      sprintf(rows[0], "row%ld", 50+i);
      pk_objs_start[0].set_varbinary(ObString::make_string(rows[0]));
      pk_objs_start[1].set_min_value();
      pk_objs_start[2].set_min_value();
      pk_objs_end[0].set_varbinary(ObString::make_string(rows[0]));
      pk_objs_end[1].set_max_value();
      pk_objs_end[2].set_max_value();
      range.start_key_.assign(pk_objs_start, 3);
      range.end_key_.assign(pk_objs_end, 3);
      range.border_flag_.set_inclusive_start();
      range.border_flag_.set_inclusive_end();
      query.clear_scan_range();
      ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));

      entity_factory.free_and_reuse();
      mutations.reset();
      // delete row iff check succeed
      key1.set_varbinary(ObString::make_string(rows[0]));
      key2.set_null();          // delete all qualifier
      key3.set_int(-INT64_MAX);  // delete latest version
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      ASSERT_EQ(OB_SUCCESS, mutations.del(*entity));
      ObTableQueryAndMutateResult result;
      int64_t &affected_rows = result.affected_rows_;
      ASSERT_EQ(OB_SUCCESS, the_table->execute_query_and_mutate(query_and_mutate, result));
      fprintf(stderr, "query_and_mutate affected_rows=%ld\n", affected_rows);
      if (i % 4 == 0) {
        ASSERT_EQ(0, affected_rows);
      } else {
        ASSERT_EQ(1, affected_rows);
      }
    }   // end for

    // scan row50 ~ row57
    pk_objs_start[0].set_varbinary(ObString::make_string("row50"));
    pk_objs_start[1].set_min_value();
    pk_objs_start[2].set_min_value();
    pk_objs_end[0].set_varbinary(ObString::make_string("row58"));
    pk_objs_end[1].set_min_value();
    pk_objs_end[2].set_min_value();
    range.start_key_.assign(pk_objs_start, 3);
    range.end_key_.assign(pk_objs_end, 3);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.unset_inclusive_end();
    query.clear_scan_range();
    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    htable_filter.clear_columns();
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string("cq0")));
    htable_filter.set_max_versions(1);
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0};
    int64_t timestamps[] = {9};
    for (int64_t i = 0; i < 2; ++i) {
      // 2 rows
      if (i == 0) {
        sprintf(rows[0], "row50");
      } else {
        sprintf(rows[0], "row54");
      }
      key1.set_varbinary(ObString::make_string(rows[0]));
      for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
        // 4 qualifier
        sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
        key2.set_varbinary(ObString::make_string(qualifier2[j]));
        for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
        {
          key3.set_int(timestamps[k]);
          ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
          ObObj rk, cq, ts, val;
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
          ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
          //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
          ASSERT_EQ(key1, rk);
          ASSERT_EQ(key2, cq);
          ASSERT_EQ(key3, ts);
        } // end for
      }
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
  delete [] rows;
}

void TestBatchExecute::generate_get(ObTableQuery &query, ObObj pk_objs_start[], ObObj pk_objs_end[], const char* rowkey)
{
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));

  pk_objs_start[0].set_varbinary(ObString::make_string(rowkey));
  pk_objs_start[1].set_min_value();
  pk_objs_start[2].set_min_value();
  pk_objs_end[0].set_varbinary(ObString::make_string(rowkey));
  pk_objs_end[1].set_max_value();
  pk_objs_end[2].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 3);
  range.end_key_.assign(pk_objs_end, 3);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();
  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
}

TEST_F(TestBatchExecute, htable_increment)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_increment"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  char values[VERSIONS_COUNT][8];
  for (int i = 0; i < VERSIONS_COUNT; ++i) {
    ObHTableUtils::int64_to_java_bytes(i*2, values[i]);
  }
  const char* rowkey = "row0";
  ObObj key1, key2, key3;
  ObObj value;
  key1.set_varbinary(ObString::make_string(rowkey));
  for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
    key2.set_varbinary(ObString::make_string(qualifier[j]));  // cq0, cq1, cq2, ... cq9
    for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
    {
      key3.set_int(k);
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      ObString str(8, values[j]);
      value.set_varbinary(str);
      ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
      ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
    } // end for
  }   // end for
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  ASSERT_EQ(1*COLUMNS_SIZE*VERSIONS_COUNT, result.count());
  ////////////////////////////////////////////////////////////////
  // case
  ObTableQueryAndMutate query_and_mutate;
  ObTableQuery &query = query_and_mutate.get_query();
  ObTableBatchOperation &mutations = query_and_mutate.get_mutations();
  ObObj pk_objs_start[3];
  ObObj pk_objs_end[3];
  ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, rowkey));
  ObHTableFilter &htable_filter = query.htable_filter();
  int cqids[] = {8, 7, 6, 5, 4, 3, 2, 1};
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; OB_SUCCESS == ret && i < ARRAYSIZEOF(cqids); ++i)
  {
    sprintf(qualifier2[i], "cq%d", cqids[i]);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier2[i])));
  } // end for
  htable_filter.set_valid(true);

  // do increment
  mutations.reset();
  key1.set_varbinary(ObString::make_string(rowkey));
  for (int64_t j = 0; j < ARRAYSIZEOF(cqids); ++j) {
    key2.set_varbinary(ObString::make_string(qualifier2[j]));
    key3.set_int(VERSIONS_COUNT);
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ObString v_str(8, values[j]);
    value.set_varbinary(v_str);
    // 8 + 0, 7 + 2, ... 1 + 14
    ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
    ASSERT_EQ(OB_SUCCESS, mutations.increment(*entity));
  }
  ObTableQueryAndMutateResult inc_result;
  int64_t &affected_rows = inc_result.affected_rows_;
  ASSERT_EQ(OB_SUCCESS, the_table->execute_query_and_mutate(query_and_mutate, inc_result));
  fprintf(stderr, "affected_rows=%ld\n", affected_rows);
  {
    int64_t num = 0;
    int64_t new_int = 0;
    ObObj val;
    ObString str;
    const ObITableEntity *result_entity = NULL;
    while(OB_SUCC(inc_result.affected_entity_.get_next_entity(result_entity))) {
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
      ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
      ASSERT_EQ(OB_SUCCESS, ObHTableUtils::java_bytes_to_int64(str, new_int));
      //fprintf(stderr, "%s | %ld\n", S(*result_entity), new_int);
      num++;
      ASSERT_EQ(16, new_int);
    }
    ASSERT_EQ(num, ARRAYSIZEOF(cqids));
  }
  // verify result
  htable_filter.clear_columns();
  ObTableEntityIterator *iter = nullptr;
  ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
  const ObITableEntity *result_entity = NULL;
  int cqids_sorted[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  key1.set_varbinary(ObString::make_string(rowkey));
  int64_t expected_int = 0;
  ObString str;
  int64_t read_int = 0;
  for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
    sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
    key2.set_varbinary(ObString::make_string(qualifier2[j]));
    expected_int = 16;
    // version is now()
    key3.set_null();
    ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
    ObObj rk, cq, ts, val;
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
    ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
    ASSERT_EQ(OB_SUCCESS, ObHTableUtils::java_bytes_to_int64(str, read_int));
    //fprintf(stderr, "(%s,%s,%s,%s(%ld))\n", S(rk), S(cq), S(ts), S(val), read_int);
    ASSERT_EQ(key1, rk);
    ASSERT_EQ(key2, cq);
    if (j == 0) {
      ASSERT_EQ(read_int, 0);
    } else if (j == 9) {
      ASSERT_EQ(read_int, 18);
    } else {
      ASSERT_EQ(read_int, expected_int);
    }
  }  // end for
  ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));

  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

TEST_F(TestBatchExecute, htable_increment_empty)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_increment"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  char values[VERSIONS_COUNT][8];
  for (int i = 0; i < VERSIONS_COUNT; ++i) {
    ObHTableUtils::int64_to_java_bytes(i, values[i]);
  }
  const char* rowkey = "row1";
  ////////////////////////////////////////////////////////////////
  // case
  ObTableQueryAndMutate query_and_mutate;
  ObTableQuery &query = query_and_mutate.get_query();
  ObTableBatchOperation &mutations = query_and_mutate.get_mutations();
  ObObj pk_objs_start[3];
  ObObj pk_objs_end[3];
  ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, rowkey));
  ObHTableFilter &htable_filter = query.htable_filter();
  int cqids[] = {8, 7, 6, 5, 4, 3, 2, 1};
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; OB_SUCCESS == ret && i < ARRAYSIZEOF(cqids); ++i)
  {
    sprintf(qualifier2[i], "cq%d", cqids[i]);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier2[i])));
  } // end for
  htable_filter.set_valid(true);

  // do increment
  mutations.reset();
  ObObj key1, key2, key3;
  ObObj value;
  key1.set_varbinary(ObString::make_string(rowkey));
  for (int64_t j = 0; j < ARRAYSIZEOF(cqids); ++j) {
    key2.set_varbinary(ObString::make_string(qualifier2[j]));
    key3.set_int(VERSIONS_COUNT);
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ObString v_str(8, values[j]);
    value.set_varbinary(v_str);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
    ASSERT_EQ(OB_SUCCESS, mutations.increment(*entity));
  }
  ObTableQueryAndMutateResult inc_result;
  int64_t &affected_rows = inc_result.affected_rows_;
  ASSERT_EQ(OB_NOT_SUPPORTED, the_table->execute_query_and_mutate(query_and_mutate, inc_result));
  {
    // int64_t num = 0;
    // int64_t new_int = 0;
    // ObObj val;
    // ObString str;
    // const ObITableEntity *result_entity = NULL;
    // while(OB_SUCC(inc_result.affected_entity_.get_next_entity(result_entity))) {
    //   ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
    //   ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
    //   ASSERT_EQ(OB_SUCCESS, ObHTableUtils::java_bytes_to_int64(str, new_int));
    //   //fprintf(stderr, "%s | %ld\n", S(*result_entity), new_int);
    //   num++;
    //   ASSERT_EQ(8-num, new_int);
    // }
    // ASSERT_EQ(num, ARRAYSIZEOF(cqids));
  }
  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

// todo@dazhi: for update is not work in master, use htable lock to fix it
// TEST_F(TestBatchExecute, htable_increment_multi_thread)
// {
//   // setup
//   ObTable *the_table = NULL;
//   int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_increment"), the_table);
//   ASSERT_EQ(OB_SUCCESS, ret);
//   the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
//   ObTableEntityFactory<ObTableEntity> entity_factory;
//   ObTableBatchOperation batch_operation;
//   ObITableEntity *entity = NULL;

//   // 1. insert cell: row1 cq1 0 0
//   const char* rowkey = "row2";
//   const char *qualifier = "cq1";
//   char old_val[8];
//   ObHTableUtils::int64_to_java_bytes(0, old_val);
//   ObString str(8, old_val);
//   int version = 0;
//   ObObj key1, key2, key3, value;
//   key1.set_varbinary(ObString::make_string(rowkey));
//   key2.set_varbinary(ObString::make_string(qualifier));  // cq0, cq2, cq4, ... cq8
//   key3.set_int(version);
//   value.set_varbinary(str);
//   entity = entity_factory.alloc();
//   ASSERT_TRUE(NULL != entity);
//   ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
//   ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
//   ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
//   ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
//   ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
//   ObTableBatchOperationResult result;
//   ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
//   ASSERT_EQ(1, result.count());
//   // after insertion
//   // row2 cq0 0 0

//   ////////////////////////////////////////////////////////////////
//   // 2. fill query and mutations
//   // query: query with row0 and htable filter for column cq0
//   // mutation: row2, cq0, 1, 1 
//   ObTableQueryAndMutate query_and_mutate;
//   ObTableQuery &query = query_and_mutate.get_query();
//   ObTableBatchOperation &mutations = query_and_mutate.get_mutations();
//   ObObj pk_objs_start[3];
//   ObObj pk_objs_end[3];
//   ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, rowkey));
//   ObHTableFilter &htable_filter = query.htable_filter();
//   const char *qualifier2 = qualifier;
//   htable_filter.add_column(ObString::make_string(qualifier2));
//   ASSERT_EQ(1, htable_filter.get_max_versions());
//   htable_filter.set_valid(true);

//   mutations.reset();
//   key1.set_varbinary(ObString::make_string(rowkey));
//   key2.set_varbinary(ObString::make_string(qualifier2));
//   key3.set_int(version + 1);
//   entity = entity_factory.alloc();
//   ASSERT_TRUE(NULL != entity);
//   ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
//   ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
//   ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
//   char inc_value[8];
//   ObHTableUtils::int64_to_java_bytes(1, inc_value);
//   ObString inc_str(8, inc_value);
//   value.set_varbinary(inc_str);
//   ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
//   ASSERT_EQ(OB_SUCCESS, mutations.increment(*entity));

//   // 3. execute increment and check result
//   auto task = [&](ObTable* one_table, const ObTableQueryAndMutate &query_and_mutate, uint64_t inc_times) {
//     ObTableQueryAndMutateResult inc_result;
//     for (int i = 0; i < inc_times; i++) {
//       ASSERT_EQ(OB_SUCCESS, one_table->execute_query_and_mutate(query_and_mutate, inc_result));
//       ASSERT_EQ(1, inc_result.affected_rows_);
//     }
//   };
//   constexpr uint64_t thread_num = 30;
//   constexpr uint64_t inc_times = 50;
//   std::vector<std::thread> threads;
//   time_t start_time = time(NULL);
//   printf("begin to run tasks, thread_num: %ld\n", thread_num);
//   for (uint64_t i = 0; i < thread_num; ++i) {
//     std::thread t(task, the_table, query_and_mutate, inc_times);
//     threads.push_back(std::move(t));
//   }
//   for (uint64_t i = 0; i < thread_num; ++i) {
//     threads.at(i).join();
//   }
//   printf("time elapsed during query process: %lfs\n", double(time(NULL) - start_time));

  
//   // 4. execute query and verify result
//   htable_filter.clear_columns();
//   ObTableEntityIterator *iter = nullptr;
//   ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
//   const ObITableEntity *result_entity = NULL;
//   key1.set_varbinary(ObString::make_string(rowkey));
//   key2.set_varbinary(ObString::make_string(qualifier2));
//   key3.set_int(version);
//   int64_t read_int = 0;
//   ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
//   ObObj rk, cq, ts, val;
//   ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
//   ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
//   ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
//   ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
//   ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
//   ASSERT_EQ(OB_SUCCESS, ObHTableUtils::java_bytes_to_int64(str, read_int));
//   ASSERT_EQ(key1, rk);
//   ASSERT_EQ(key2, cq);
//   // ASSERT_EQ(key3, ts);
//   std::cout << "key3: " << ts.get_int() << std::endl;
//   ASSERT_EQ(read_int, thread_num * inc_times);
//   ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));

//   ////////////////////////////////////////////////////////////////
//   // teardown
//   service_client_->free_table(the_table);
//   the_table = NULL;
// }

TEST_F(TestBatchExecute, htable_append)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_append"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  char values[VERSIONS_COUNT][128];
  for (int i = 0; i < VERSIONS_COUNT; ++i) {
    sprintf(values[i], "ob%d", i);
  }
  const char* rowkey = "row0";
  ObObj key1, key2, key3;
  ObObj value;
  key1.set_varbinary(ObString::make_string(rowkey));
  for (int64_t j = 0; j < COLUMNS_SIZE/2; ++j) {
    key2.set_varbinary(ObString::make_string(qualifier[j*2]));  // cq0, cq2, cq4, ... cq8
    for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
    {
      key3.set_int(k);
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      value.set_varbinary(ObString::make_string(values[j*2]));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
      ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
    } // end for
  }   // end for
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  ASSERT_EQ(1*COLUMNS_SIZE/2*VERSIONS_COUNT, result.count());
  ////////////////////////////////////////////////////////////////
  // case
  ObTableQueryAndMutate query_and_mutate;
  ObTableQuery &query = query_and_mutate.get_query();
  ObTableBatchOperation &mutations = query_and_mutate.get_mutations();
  ObObj pk_objs_start[3];
  ObObj pk_objs_end[3];
  ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, rowkey));
  ObHTableFilter &htable_filter = query.htable_filter();
  int cqids[] = {8, 7, 6, 5, 4, 3, 2, 1};
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; OB_SUCCESS == ret && i < ARRAYSIZEOF(cqids); ++i)
  {
    sprintf(qualifier2[i], "cq%d", cqids[i]);
    ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string(qualifier2[i])));
  } // end for
  htable_filter.set_valid(true);

  // do append
  mutations.reset();
  key1.set_varbinary(ObString::make_string(rowkey));
  for (int64_t j = 0; j < ARRAYSIZEOF(cqids); ++j) {
    key2.set_varbinary(ObString::make_string(qualifier2[j]));
    key3.set_int(VERSIONS_COUNT);
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ObString v_str = ObString::make_string(values[j]);
    value.set_varbinary(v_str);
    // 8 + 0, 7 + 1, ... 1 + 7
    ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
    ASSERT_EQ(OB_SUCCESS, mutations.append(*entity));
  }
  ObTableQueryAndMutateResult inc_result;
  int64_t &affected_rows = inc_result.affected_rows_;
  ASSERT_EQ(OB_SUCCESS, the_table->execute_query_and_mutate(query_and_mutate, inc_result));
  fprintf(stderr, "affected_rows=%ld\n", affected_rows);
  {
    int64_t num = 0;
    ObObj val;
    ObString str;
    const ObITableEntity *result_entity = NULL;
    while(OB_SUCC(inc_result.affected_entity_.get_next_entity(result_entity))) {
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
      ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
      fprintf(stderr, "%s\n", S(*result_entity));
      num++;
      // if (num % 2 == 0) {
      //   ASSERT_EQ(8, new_int);
      // } else {
      //   ASSERT_EQ(8-num, new_int);
      // }
    }
    ASSERT_EQ(num, ARRAYSIZEOF(cqids));
  }
  // verify result
  htable_filter.clear_columns();
  ObTableEntityIterator *iter = nullptr;
  ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
  const ObITableEntity *result_entity = NULL;
  int cqids_sorted[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
  key1.set_varbinary(ObString::make_string(rowkey));
  char expected_str[128];
  ObString str;
  for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
    sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
    key2.set_varbinary(ObString::make_string(qualifier2[j]));
    if (j == 0) {
      sprintf(expected_str, "ob%ld", j);
      key3.set_int(9);
    } else if (j % 2==0) {
      sprintf(expected_str, "ob%ldob%ld", j, 8-j);
      // version is now()
      key3.set_null();
    } else {
      sprintf(expected_str, "ob%ld", 8-j);  // first write
      key3.set_int(VERSIONS_COUNT);
    }
    ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
    ObObj rk, cq, ts, val;
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
    ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
    fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));

    ASSERT_EQ(key1, rk);
    ASSERT_EQ(key2, cq);
    if (!key3.is_null()) {
      ASSERT_EQ(key3, ts);
    }
    ASSERT_TRUE(0 == str.compare(ObString::make_string(expected_str)));
  }  // end for
  ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));

  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

void TestBatchExecute::prepare_data(ObTable *the_table)
{
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  DefaultBuf *rows = new (std::nothrow) DefaultBuf[BATCH_SIZE];
  ASSERT_TRUE(NULL != rows);
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  ObObj key1, key2, key3;
  ObObj value;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    sprintf(rows[i], "row%ld", i);
    key1.set_varbinary(ObString::make_string(rows[i]));
    for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
      key2.set_varbinary(ObString::make_string(qualifier[j]));
      for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
      {
        key3.set_int(k);
        entity = entity_factory.alloc();
        ASSERT_TRUE(NULL != entity);
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
        ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
        switch (i % 4) {
          case 0:
            value.set_varbinary(ObString::make_string("string2"));
            break;
          case 1:
            value.set_varbinary(ObString::make_string("string3"));
            break;
          case 2:  // row50
            value.set_varbinary(ObString::make_string("string0"));
            break;
          case 3:
            value.set_varbinary(ObString::make_string("string1"));
            break;
          default:
            ASSERT_TRUE(0);
        }
        ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
        ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
      } // end for
    }   // end for
  }     // end for
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(1, result.count());
  ASSERT_EQ(OB_SUCCESS, result.at(0).get_errno());
  delete [] rows;
}

TEST_F(TestBatchExecute, htable_mutations)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_mutate"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ASSERT_NO_FATAL_FAILURE(prepare_data(the_table));
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  ////////////////////////////////////////////////////////////////
  // row_mutation (cq0->haha, delete cq3, cq5, cq5->kaka)
  ObObj key1, key2, key3, value;
  key1.set_varbinary(ObString::make_string("row0"));
  key2.set_varbinary(ObString::make_string("cq0"));
  key3.set_int(INT64_MAX);
  value.set_varbinary(ObString::make_string("haha"));
  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
  ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
  ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));

  static constexpr int64_t COLUMNS_SIZE = 10;
  key1.set_varbinary(ObString::make_string("row0"));
  int cqids[] = {3, 5};
  char qualifier2[COLUMNS_SIZE][128];
  for (int64_t j = 0; j < ARRAYSIZEOF(cqids); ++j) {
    sprintf(qualifier2[j], "cq%d", cqids[j]);
    key2.set_varbinary(ObString::make_string(qualifier2[j]));
    key3.set_int(-INT64_MAX);  // delete all version
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));
  }

  key1.set_varbinary(ObString::make_string("row0"));
  key2.set_varbinary(ObString::make_string("cq5"));
  key3.set_int(INT64_MAX);
  value.set_varbinary(ObString::make_string("kaka"));
  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
  ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
  ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
  ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  ASSERT_EQ(1, result.count());
  ASSERT_EQ(OB_SUCCESS, result.at(0).get_errno());
  ////////////////////////////////////////////////////////////////
  // verify
  ObTableQuery query;
  ObObj pk_objs_start[3];
  ObObj pk_objs_end[3];
  ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, "row0"));
  ObHTableFilter &htable_filter = query.htable_filter();
  htable_filter.set_valid(true);

  ObTableEntityIterator *iter = nullptr;
  ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
  const ObITableEntity *result_entity = NULL;
  int cqids_sorted[] =   { 0, 1, 2, 4, 5, 6, 7, 8, 9};
  int64_t timestamps[] = {-1, 9, 9, 9, -1, 9, 9, 9, 9};
  const char* values[] = {"haha", "string2", "string2", "string2",
                          "kaka", "string2", "string2", "string2", "string2"};
  for (int64_t i = 0; i < ARRAYSIZEOF(cqids_sorted); ++i) {
    key1.set_varbinary(ObString::make_string("row0"));
    sprintf(qualifier2[i], "cq%d", cqids_sorted[i]);
    key2.set_varbinary(ObString::make_string(qualifier2[i]));
    key3.set_int(timestamps[i]);
    value.set_varbinary(ObString::make_string(values[i]));
    ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
    ObObj rk, cq, ts, val;
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
    ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
    fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
    ASSERT_EQ(key1, rk);
    ASSERT_EQ(key2, cq);
    if (timestamps[i] >= 0) {
      ASSERT_EQ(key3, ts);
    }
    ASSERT_EQ(value, val);
  } // end for
  ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

TEST_F(TestBatchExecute, hcolumn_desc)
{
  ObHColumnDescriptor column_desc;
  ObString str = ObString::make_string("{\"HColumnDescriptor\": {\"TimeToLive\": 3600}}");
  ASSERT_EQ(OB_SUCCESS, column_desc.from_string(str));
  fprintf(stderr, "ttl=%d\n", column_desc.get_time_to_live());
  ASSERT_EQ(3600, column_desc.get_time_to_live());
}


TEST_F(TestBatchExecute, htable_ttl)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_ttl"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  static constexpr int64_t VERSIONS_COUNT = 10;
  static constexpr int64_t COLUMNS_SIZE = 10;
  char qualifier[COLUMNS_SIZE][128];
  char qualifier2[COLUMNS_SIZE][128];
  for (int i = 0; i < COLUMNS_SIZE; ++i)
  {
    sprintf(qualifier[i], "cq%d", i);
  } // end for
  char values[VERSIONS_COUNT][128];
  for (int i = 0; i < VERSIONS_COUNT; ++i) {
    sprintf(values[i], "ob%d", i);
  }
  const char* rowkey = "row0";
  ObObj key1, key2, key3;
  ObObj value;
  key1.set_varbinary(ObString::make_string(rowkey));
  for (int64_t j = 0; j < COLUMNS_SIZE; ++j) {
    key2.set_varbinary(ObString::make_string(qualifier[j]));  // cq0 ~ cq9
    key3.set_int(INT64_MAX);  // now
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    value.set_varbinary(ObString::make_string(values[j]));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
  }   // end for
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  ASSERT_EQ(1, result.count());
  fprintf(stderr, "sleep 3\n");
  sleep(3);  // ttl is 5
  {
    // verify
    ObTableQuery query;
    ObObj pk_objs_start[3];
    ObObj pk_objs_end[3];
    ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, rowkey));

    ObHTableFilter &htable_filter = query.htable_filter();
    htable_filter.set_valid(true);
    htable_filter.clear_columns();
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    char expected_str[128];
    ObString str;
    key1.set_varbinary(ObString::make_string(rowkey));
    for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
      sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
      key2.set_varbinary(ObString::make_string(qualifier2[j]));
      sprintf(expected_str, "ob%ld", j);

      ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
      ObObj rk, cq, ts, val;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
      ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
      fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));

      ASSERT_EQ(key1, rk);
      ASSERT_EQ(key2, cq);
      ASSERT_TRUE(0 == str.compare(ObString::make_string(expected_str)));
    } // end for
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  // insert new value for cq0, cq2, cq4, ... cq8
  entity_factory.free_and_reuse();
  batch_operation.reset();
  for (int64_t j = 0; j < COLUMNS_SIZE/2; ++j) {
    key2.set_varbinary(ObString::make_string(qualifier[j*2]));
    key3.set_int(INT64_MAX);
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    value.set_varbinary(ObString::make_string(values[j*2]));
    ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
  }   // end for
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  ASSERT_EQ(1, result.count());

  // sleep for 3 seconds
  fprintf(stderr, "sleep 3\n");
  sleep(3);  // ttl is 5
  {
    // verify
    ObTableQuery query;
    ObObj pk_objs_start[3];
    ObObj pk_objs_end[3];
    ASSERT_NO_FATAL_FAILURE(generate_get(query, pk_objs_start, pk_objs_end, rowkey));

    ObHTableFilter &htable_filter = query.htable_filter();
    htable_filter.set_valid(true);
    htable_filter.clear_columns();
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int cqids_sorted[] = {0, 2, 4, 6, 8};
    char expected_str[128];
    ObString str;
    key1.set_varbinary(ObString::make_string(rowkey));
    for (int64_t j = 0; j < ARRAYSIZEOF(cqids_sorted); ++j) {
      sprintf(qualifier2[j], "cq%d", cqids_sorted[j]);
      key2.set_varbinary(ObString::make_string(qualifier2[j]));
      sprintf(expected_str, "ob%ld", j*2);

      ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
      ObObj rk, cq, ts, val;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
      ASSERT_EQ(OB_SUCCESS, val.get_varbinary(str));
      fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));

      ASSERT_EQ(key1, rk);
      ASSERT_EQ(key2, cq);
      ASSERT_TRUE(0 == str.compare(ObString::make_string(expected_str)));
    } // end for
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }

  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

// TEST_F(TestBatchExecute, secondary_index)
// {
//   OB_LOG(INFO, "begin secondary_index");
//   ObTable *the_table = NULL;
//   int ret = service_client_->alloc_table(ObString::make_string("secondary_index_test"), the_table);
//   ASSERT_EQ(OB_SUCCESS, ret);
//   ObTableEntityFactory<ObTableEntity> entity_factory;
//   ObTableBatchOperation batch_operation;
//   ObITableEntity *entity = NULL;
//
//   //
//   // >  select * from secondary_index_test limit 10;
//   // +----+------+------+
//   // | C1 | C2   | C3   |
//   // +----+------+------+
//   // |  0 |    0 | aaa  |
//   // |  2 |    1 | xxx  |
//   // |  4 |    2 | yyy  |
//   // |  6 |    3 | zzz  |
//   // |  8 |    0 | AAA  |
//   // | 10 |    1 | XXX  |
//   // | 12 |    2 | YYY  |
//   // | 14 |    3 | ZZZ  |
//   // | 16 |    0 | aaa  |
//   // | 18 |    1 | xxx  |
//   // +----+------+------+
//
//   const char *c3_values[] = {"aaa", "xxx", "yyy", "zzz", "AAA", "XXX", "YYY", "ZZZ"};
//   const int64_t c3_values_count = ARRAYSIZEOF(c3_values);
//   for (int64_t i = 0; i < BATCH_SIZE; ++i) {
//     entity = entity_factory.alloc();
//     ASSERT_TRUE(NULL != entity);
//     ObObj key;
//     key.set_int(i*2);
//     ObObj value;
//     value.set_int(i%4);
//     ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
//     ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
//     value.set_varchar(ObString::make_string(c3_values[i%c3_values_count]));
//     value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
//     ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
//     ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
//   }
//   ASSERT_TRUE(!batch_operation.is_readonly());
//   ASSERT_TRUE(batch_operation.is_same_type());
//   ASSERT_TRUE(batch_operation.is_same_properties_names());
//   ObTableBatchOperationResult result;
//   ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
//   ASSERT_EQ(BATCH_SIZE, result.count());
//   for (int64_t i = 0; i < BATCH_SIZE; ++i)
//   {
//     const ObTableOperationResult &r = result.at(i);
//     ASSERT_EQ(OB_SUCCESS, r.get_errno());
//     ASSERT_EQ(ObTableOperationType::INSERT, r.type());
//     ASSERT_EQ(1, r.get_affected_rows());
//     const ObITableEntity *result_entity = NULL;
//     ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
//     ASSERT_TRUE(result_entity->is_empty());
//   }
//   // query using index i1(c2)
//   {
//     ObTableQuery query;
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
//     ObObj pk_objs_start[1];
//     pk_objs_start[0].set_int(1);
//     ObObj pk_objs_end[1];
//     pk_objs_end[0].set_int(2);
//     ObNewRange range;
//     range.start_key_.assign(pk_objs_start, 1);
//     range.end_key_.assign(pk_objs_end, 1);
//     range.border_flag_.set_inclusive_start();
//     range.border_flag_.set_inclusive_end();
//
//     ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
//     query.set_scan_index(ObString::make_string("i1"));
//     ObTableEntityIterator *iter = nullptr;
//     ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
//     const ObITableEntity *result_entity = NULL;
//     ObObj obj1, obj2, obj3;
//     ObString str;
//     for (int64_t i = 0; i < BATCH_SIZE/2; ++i)
//     {
//       ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
//       ASSERT_EQ(3, result_entity->get_properties_count());
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C1, obj1));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, obj2));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, obj3));
//       //fprintf(stderr, "%ld: (%s,%s,%s)\n", i, S(obj1), S(obj2), S(obj3));
//       ASSERT_EQ(OB_SUCCESS, obj3.get_varchar(str));
//       if (i < BATCH_SIZE/4) {
//         ASSERT_EQ(2+i*8, obj1.get_int());
//         ASSERT_EQ(1 , obj2.get_int());
//         ASSERT_TRUE(str == ObString::make_string(c3_values[(i%2==0)?1:5]));
//       } else {
//         ASSERT_EQ(4+(i-BATCH_SIZE/4)*8, obj1.get_int());
//         ASSERT_EQ(2 , obj2.get_int());
//         ASSERT_TRUE(str == ObString::make_string(c3_values[((i-BATCH_SIZE/4)%2==0)?2:6]));
//       }
//     }
//     ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
//   }
//   // query using index i2(c3)
//   {
//     ObTableQuery query;
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
//     ObObj pk_objs_start[1];
//     pk_objs_start[0].set_varchar(ObString::make_string("xxx"));
//     pk_objs_start[0].set_collation_type(CS_TYPE_UTF8MB4_BIN);
//     ObObj pk_objs_end[1];
//     pk_objs_end[0].set_varchar(ObString::make_string("xxx"));
//     pk_objs_end[0].set_collation_type(CS_TYPE_UTF8MB4_BIN);
//     ObNewRange range;
//     range.start_key_.assign(pk_objs_start, 1);
//     range.end_key_.assign(pk_objs_end, 1);
//     range.border_flag_.set_inclusive_start();
//     range.border_flag_.set_inclusive_end();
//
//     ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
//     query.set_scan_index(ObString::make_string("i2"));
//     ObTableEntityIterator *iter = nullptr;
//     ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
//     const ObITableEntity *result_entity = NULL;
//     ObObj obj1, obj2, obj3;
//     ObString str;
//     for (int64_t i = 0; i < BATCH_SIZE/4; ++i)
//     {
//       ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
//       ASSERT_EQ(3, result_entity->get_properties_count());
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C1, obj1));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, obj2));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, obj3));
//       //fprintf(stderr, "%ld: (%s,%s,%s)\n", i, S(obj1), S(obj2), S(obj3));
//       ASSERT_EQ(OB_SUCCESS, obj3.get_varchar(str));
//       ASSERT_EQ(2+i*8, obj1.get_int());
//       ASSERT_EQ(1 , obj2.get_int());
//       ASSERT_TRUE(str == ObString::make_string(c3_values[(i%2==0)?1:5]));
//     }
//     ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
//   }
//   // query using index i3(c2,c3)
//   {
//     ObTableQuery query;
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
//     ObObj pk_objs_start[2];
//     pk_objs_start[0].set_int(1);
//     pk_objs_start[1].set_varchar(ObString::make_string("xxx"));
//     pk_objs_start[1].set_collation_type(CS_TYPE_UTF8MB4_BIN);
//     ObObj pk_objs_end[2];
//     pk_objs_end[0].set_int(1);
//     pk_objs_end[1].set_varchar(ObString::make_string("xxx"));
//     pk_objs_end[1].set_collation_type(CS_TYPE_UTF8MB4_BIN);
//     ObNewRange range;
//     range.start_key_.assign(pk_objs_start, 2);
//     range.end_key_.assign(pk_objs_end, 2);
//     range.border_flag_.set_inclusive_start();
//     range.border_flag_.set_inclusive_end();
//
//     ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
//     query.set_scan_index(ObString::make_string("i3"));
//     ObTableEntityIterator *iter = nullptr;
//     ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
//     const ObITableEntity *result_entity = NULL;
//     ObObj obj1, obj2, obj3;
//     ObString str;
//     for (int64_t i = 0; i < BATCH_SIZE/4; ++i)
//     {
//       ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
//       ASSERT_EQ(3, result_entity->get_properties_count());
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C1, obj1));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, obj2));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, obj3));
//       //fprintf(stderr, "%ld: (%s,%s,%s)\n", i, S(obj1), S(obj2), S(obj3));
//       ASSERT_EQ(OB_SUCCESS, obj3.get_varchar(str));
//       ASSERT_EQ(2+i*8, obj1.get_int());
//       ASSERT_EQ(1 , obj2.get_int());
//       ASSERT_TRUE(str == ObString::make_string(c3_values[(i%2==0)?1:5]));
//     }
//     ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
//   }
//   // 删除后一半row
//   batch_operation.reset();
//   result.reset();
//   for (int64_t i = 0; i < BATCH_SIZE/2; ++i) {
//     entity = entity_factory.alloc();
//     ASSERT_TRUE(NULL != entity);
//     ObObj key;
//     key.set_int((BATCH_SIZE/2+i)*2);
//     ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
//     ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));
//   }
//   ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
//   ASSERT_EQ(BATCH_SIZE/2, result.count());
//   // then update C2 (2->0)
//   batch_operation.reset();
//   result.reset();
//   for (int64_t i = 0; i < BATCH_SIZE/2; ++i) {
//     entity = entity_factory.alloc();
//     ASSERT_TRUE(NULL != entity);
//     if (i % 4 == 2) {
//       ObObj key;
//       key.set_int(i*2);
//       ObObj value;
//       value.set_int(0);
//       ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
//       ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
//       ASSERT_EQ(OB_SUCCESS, batch_operation.update(*entity));
//     }
//   }
//   ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
//   ASSERT_EQ(BATCH_SIZE/8, result.count());
//   // query using index i1(c2) again
//   {
//     ObTableQuery query;
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
//     ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
//     ObObj pk_objs_start[1];
//     pk_objs_start[0].set_int(1);
//     ObObj pk_objs_end[1];
//     pk_objs_end[0].set_int(2);
//     ObNewRange range;
//     range.start_key_.assign(pk_objs_start, 1);
//     range.end_key_.assign(pk_objs_end, 1);
//     range.border_flag_.set_inclusive_start();
//     range.border_flag_.set_inclusive_end();
//
//     ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
//     query.set_scan_index(ObString::make_string("i1"));
//     ObTableEntityIterator *iter = nullptr;
//     ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
//     const ObITableEntity *result_entity = NULL;
//     ObObj obj1, obj2, obj3;
//     ObString str;
//     for (int64_t i = 0; i < 13; ++i)
//     {
//       ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
//       ASSERT_EQ(3, result_entity->get_properties_count());
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C1, obj1));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, obj2));
//       ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, obj3));
//       //fprintf(stderr, "%ld: (%s,%s,%s)\n", i, S(obj1), S(obj2), S(obj3));
//       ASSERT_EQ(OB_SUCCESS, obj3.get_varchar(str));
//       ASSERT_EQ(2+i*8, obj1.get_int());
//       ASSERT_EQ(1 , obj2.get_int());
//       ASSERT_TRUE(str == ObString::make_string(c3_values[(i%2==0)?1:5]));
//     }
//     ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
//   }
//   ////////////////////////////////////////////////////////////////
//   // teardown
//   service_client_->free_table(the_table);
//   the_table = NULL;
// }

TEST_F(TestBatchExecute, htable_empty_qualifier)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("htable1_cf1_empty_cq"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  the_table->set_entity_type(ObTableEntityType::ET_HKV);  // important
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  static constexpr int64_t VERSIONS_COUNT = 10;
  DefaultBuf *rows = new (std::nothrow) DefaultBuf[BATCH_SIZE];
  ASSERT_TRUE(NULL != rows);
  ObObj key1, key2, key3;
  ObObj value;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    sprintf(rows[i], "row%ld", i);
    key1.set_varbinary(ObString::make_string(rows[i]));
    key2.set_varbinary(ObString::make_string(""));  // empty qualifier
    for (int64_t k = 0; k < VERSIONS_COUNT; ++k)
    {
      key3.set_int(k);
      entity = entity_factory.alloc();
      ASSERT_TRUE(NULL != entity);
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
      ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
      value.set_varbinary(ObString::make_string("value_string"));
      ASSERT_EQ(OB_SUCCESS, entity->set_property(V, value));
      ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
    }   // end for
  }     // end for

  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  OB_LOG(INFO, "batch execute result", K(result));
  ASSERT_EQ(BATCH_SIZE*VERSIONS_COUNT, result.count());
  ////////////////////////////////////////////////////////////////
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(Q));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(T));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(V));
  ObObj pk_objs_start[3];
  pk_objs_start[0].set_varbinary(ObString::make_string("row50"));
  pk_objs_start[1].set_min_value();
  pk_objs_start[2].set_min_value();
  ObObj pk_objs_end[3];
  pk_objs_end[0].set_varbinary(ObString::make_string("row59"));
  pk_objs_end[1].set_max_value();
  pk_objs_end[2].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 3);
  range.end_key_.assign(pk_objs_end, 3);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();
  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));

  ObHTableFilter &htable_filter = query.htable_filter();
  ASSERT_EQ(OB_SUCCESS, htable_filter.add_column(ObString::make_string("")));
  htable_filter.set_max_versions(2);
  htable_filter.set_valid(true);
  {
    // verify put result
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int64_t timestamps[] = {9, 8};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      key2.set_varbinary(ObString::make_string(""));
      for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
      {
        key3.set_int(timestamps[k]);
        ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
        ObObj rk, cq, ts, val;
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
        //fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
        ASSERT_EQ(key1, rk);
        ASSERT_EQ(key2, cq);
        ASSERT_EQ(key3, ts);
      } // end for
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  {
    // delete row50 by qualifier and version
    const char* rowkey = "row50";
    const char* cq = "";
    int64_t ts = 8;
    batch_operation.reset();
    sprintf(rows[0], "%s", rowkey);
    key1.set_varbinary(ObString::make_string(rows[0]));
    key2.set_varbinary(ObString::make_string(cq));
    key3.set_int(ts);  // delete the specified version
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key1));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key2));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key3));
    ASSERT_EQ(OB_SUCCESS, batch_operation.del(*entity));

    ObTableBatchOperationResult result;
    ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  }

  {
    // verify delete result
    ObTableEntityIterator *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    const ObITableEntity *result_entity = NULL;
    int64_t timestamps[] = {9, 8};
    for (int64_t i = 0; i < 10; ++i) {
      // 10 rowkeys
      sprintf(rows[i], "row%ld", i+50);
      key1.set_varbinary(ObString::make_string(rows[i]));
      key2.set_varbinary(ObString::make_string(""));
      for (int64_t k = 0; k < ARRAYSIZEOF(timestamps); ++k)
      {
        if (0 == i && k == 1) {
          key3.set_int(7);
        } else {
          key3.set_int(timestamps[k]);
        }
        ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
        ObObj rk, cq, ts, val;
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, rk));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(Q, cq));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(T, ts));
        ASSERT_EQ(OB_SUCCESS, result_entity->get_property(V, val));
        // fprintf(stderr, "(%s,%s,%s,%s)\n", S(rk), S(cq), S(ts), S(val));
        ASSERT_EQ(key1, rk);
        ASSERT_EQ(key2, cq);
        ASSERT_EQ(key3, ts);
      } // end for
    }
    ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  }
  ////////////////////////////////////////////////////////////////
  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
  delete [] rows;
}

#define T_CASE_SUCC(SRC_TYPE, SRC_VAL, DELTA_TYPE, DELTA_VAL, RES_TYPE, RES_TYPE2, RES_VAL) \
  src.set_##SRC_TYPE(SRC_VAL);\
  delta.set_##DELTA_TYPE(DELTA_VAL);\
  target_type.set_type(src.get_type());         \
  target.set_null();\
  ASSERT_EQ(OB_SUCCESS, ObTableService::obj_increment(delta, src, target_type, target)); \
  ASSERT_EQ(Ob##RES_TYPE##Type, target.get_type());                          \
  ASSERT_EQ(RES_VAL, target.get_##RES_TYPE2());\
  fprintf(stderr, "CASE: " #SRC_TYPE "(" #SRC_VAL ") + " #DELTA_TYPE "(" #DELTA_VAL ") = " #RES_TYPE2 "(" #RES_VAL ")\n");

#define T_CASE_FAIL(SRC_TYPE, SRC_VAL, DELTA_TYPE, DELTA_VAL, RES) \
  src.set_##SRC_TYPE(SRC_VAL);\
  delta.set_##DELTA_TYPE(DELTA_VAL);\
  target_type.set_type(src.get_type());         \
  target.set_null();\
  ASSERT_EQ(RES, ObTableService::obj_increment(delta, src, target_type, target)); \
  ASSERT_TRUE(target.is_null());\
  fprintf(stderr, "CASE: " #SRC_TYPE "(" #SRC_VAL ") + " #DELTA_TYPE "(" #DELTA_VAL ") = " #RES "\n");


#define T_CASE_APP(SRC_VAL, SRC_CS_TYPE, DELTA_VAL, DELTA_CS_TYPE, RES_VAL, RES_CS_TYPE)\
  src.set_varchar(ObString::make_string(SRC_VAL));\
  src.set_collation_type(SRC_CS_TYPE);\
  delta.set_varchar(ObString::make_string(DELTA_VAL));\
  delta.set_collation_type(DELTA_CS_TYPE);\
  target_type.set_type(ObVarcharType);\
  target_type.set_collation_type(src.get_collation_type());     \
  target.set_null();\
  alloc.reset();    \
  ASSERT_EQ(OB_SUCCESS, ObTableService::obj_append(delta, src, target_type, alloc, target)); \
  ASSERT_EQ(ObVarcharType, target.get_type());\
  ASSERT_EQ(RES_CS_TYPE, target.get_collation_type());\
  ASSERT_TRUE(ObString::make_string(RES_VAL) == target.get_varchar());\
  fprintf(stderr, "CASE: " #SRC_CS_TYPE "(" #SRC_VAL ") || " #DELTA_CS_TYPE "(" #DELTA_VAL ") = " #RES_VAL "\n");


//TEST_F(TestBatchExecute, increment_and_append)
//{
  //// insert C2
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObITableEntity *entity = NULL;
  //entity = entity_factory.alloc();
  //ASSERT_TRUE(NULL != entity);
  //int64_t key_key = 10;
  //ObObj key;
  //key.set_int(key_key);
  //int64_t value_value = 1000;
  //ObObj c2_obj;
  //c2_obj.set_int(value_value);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, c2_obj));
  //ObString c3_value = ObString::make_string("hello world");
  //ObObj c3_obj;
  //c3_obj.set_varchar(c3_value);
  //c3_obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, c3_obj));
  //ObTableOperation table_operation = ObTableOperation::insert(*entity);
  //ObTableOperationResult r;
  //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
  //const ObITableEntity *result_entity = NULL;
  //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //ASSERT_EQ(1, r.get_affected_rows());
  //// insert another row (11, 1000, null)
  //entity->reset();
  //key.set_int(key_key+1);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, c2_obj));
  //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
  //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //ASSERT_EQ(1, r.get_affected_rows());
  //// insert another row (12, null, "hello world")
  //entity->reset();
  //key.set_int(key_key+2);
  //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
  //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, c3_obj));
  //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
  //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //ASSERT_EQ(1, r.get_affected_rows());
  //ObObj value;
  //// append C3
  //{
    //entity->reset();
    //key.set_int(key_key);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //ObTableOperation table_operation = ObTableOperation::append(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(true);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::APPEND, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(NULL != result_entity);
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(1, result_entity->get_rowkey_size());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_rowkey_value(0, value));
    //ASSERT_EQ(key_key, value.get_int());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello worldhello world");
    //ASSERT_TRUE(str == c3_new_value);
    //ASSERT_EQ(CS_TYPE_UTF8MB4_GENERAL_CI, value.get_collation_type());
  //}
  //// increment C2
  //{
    //entity->reset();
    //key.set_int(key_key);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_int(111);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //ObTableOperation table_operation = ObTableOperation::increment(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(false);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::INCREMENT, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(value_value+111, value.get_int());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //key.set_int(key_key);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(value_value+111, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello worldhello world");
    //ASSERT_TRUE(str == c3_new_value);
  //}
  //// append to null column
  //{
    //entity->reset();
    //key.set_int(key_key+1);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //ObTableOperation table_operation = ObTableOperation::append(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(true);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::APPEND, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(1, result_entity->get_rowkey_size());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_rowkey_value(0, value));
    //ASSERT_EQ(key_key+1, value.get_int());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello world");
    //ASSERT_TRUE(str == c3_new_value);
    //ASSERT_EQ(CS_TYPE_UTF8MB4_GENERAL_CI, value.get_collation_type());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //key.set_int(key_key+1);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(value_value, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello world");
    //ASSERT_TRUE(str == c3_new_value);
  //}
  //// increment null value
  //{
    //entity->reset();
    //key.set_int(key_key+2);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_int(111);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //ObTableOperation table_operation = ObTableOperation::increment(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(false);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::INCREMENT, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(111, value.get_int());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //key.set_int(key_key+2);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(111, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello world");
    //ASSERT_TRUE(str == c3_new_value);
  //}
  //// increment row not exist
  //{
    //entity->reset();
    //key.set_int(key_key+3);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_int(111);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //ObTableOperation table_operation = ObTableOperation::increment(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(false);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::INCREMENT, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(111, value.get_int());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //key.set_int(key_key+3);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_EQ(111, value.get_int());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ASSERT_TRUE(value.is_null());
  //}
  //// append to row not exist
  //{
    //entity->reset();
    //key.set_int(key_key+4);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //ObTableOperation table_operation = ObTableOperation::append(*entity);
    //ObTableRequestOptions req_options;
    //req_options.set_returning_affected_entity(true);
    //req_options.set_returning_rowkey(true);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, req_options, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::APPEND, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(!result_entity->is_empty());
    //ASSERT_EQ(1, result_entity->get_rowkey_size());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_rowkey_value(0, value));
    //ASSERT_EQ(key_key+4, value.get_int());
    //ASSERT_EQ(1, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello world");
    //ASSERT_TRUE(str == c3_new_value);
    //ASSERT_EQ(CS_TYPE_UTF8MB4_GENERAL_CI, value.get_collation_type());
  //}
  //// get
  //{
    //ObObj null_obj;
    //entity->reset();
    //key.set_int(key_key+4);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
    //ObTableOperation table_operation = ObTableOperation::retrieve(*entity);
    //ASSERT_EQ(OB_SUCCESS, table_->execute(table_operation, r));
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(0, r.get_affected_rows());
    //ASSERT_EQ(ObTableOperationType::GET, r.type());
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_EQ(0, result_entity->get_rowkey_size());
    //ASSERT_EQ(2, result_entity->get_properties_count());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
    //ASSERT_TRUE(value.is_null());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
    //ObString str;
    //ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
    //ObString c3_new_value = ObString::make_string("hello world");
    //ASSERT_TRUE(str == c3_new_value);
  //}
//}

//TEST_F(TestBatchExecute, increment_and_append_batch)
//{
  //// prepare
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObTableBatchOperation batch_operation;
  //ObITableEntity *entity = NULL;
  //ObString c3_value = ObString::make_string("hello world");
  //ObString c3_new_value = ObString::make_string("hello worldhello world");
  //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    //entity = entity_factory.alloc();
    //ASSERT_TRUE(NULL != entity);
    //ObObj key;
    //key.set_int(i*2);
    //ObObj value;
    //value.set_int(100+i);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    //value.set_varchar(c3_value);
    //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    //ASSERT_EQ(OB_SUCCESS, batch_operation.insert_or_update(*entity));
  //}
  //ASSERT_TRUE(!batch_operation.is_readonly());
  //ASSERT_TRUE(batch_operation.is_same_type());
  //ASSERT_TRUE(batch_operation.is_same_properties_names());
  //ObTableBatchOperationResult result;
  //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
  //ASSERT_EQ(BATCH_SIZE, result.count());
  //// case
  //batch_operation.reset();
  //entity_factory.free_and_reuse();
  //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    //entity = entity_factory.alloc();
    //ASSERT_TRUE(NULL != entity);
    //ObObj key;
    //key.set_int(i*2);
    //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    //ObObj value;
    //if (0 == i % 2) {
      //value.set_int(i);
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.increment(*entity));
    //} else if (1 == i % 2) {
      //value.set_varchar(c3_value);
      //value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.append(*entity));
    //}
  //}
  //ASSERT_TRUE(!batch_operation.is_readonly());
  //ASSERT_TRUE(!batch_operation.is_same_type());
  //ASSERT_TRUE(!batch_operation.is_same_properties_names());
  //ObTableRequestOptions req_options;
  //req_options.set_returning_affected_entity(true);
  //req_options.set_returning_rowkey(true);
  //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, req_options, result));
  //OB_LOG(INFO, "batch execute result", K(result));
  //ASSERT_EQ(BATCH_SIZE, result.count());
  //for (int64_t i = 0; i < BATCH_SIZE; ++i)
  //{
    //const ObTableOperationResult &r = result.at(i);
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
    //ASSERT_EQ(1, r.get_affected_rows());
    //if (0 == i % 2) {
      //ASSERT_EQ(ObTableOperationType::INCREMENT, r.type());
    //} else {
      //ASSERT_EQ(ObTableOperationType::APPEND, r.type());
    //}
    //const ObITableEntity *result_entity = NULL;
    //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    //ASSERT_TRUE(!result_entity->is_empty());
    //ObObj value;
    //ASSERT_EQ(1, result_entity->get_rowkey_size());
    //ASSERT_EQ(OB_SUCCESS, result_entity->get_rowkey_value(0, value));
    //ASSERT_EQ(i*2, value.get_int());

    //ObObj c2_obj, c3_obj;
    //ObObj val2, val3;
    //if (0 == i % 2) {
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, c2_obj));
      //ASSERT_EQ(OB_HASH_NOT_EXIST, result_entity->get_property(C3, c3_obj));
      //val2.set_int(100+2*i);
      //ASSERT_EQ(val2, c2_obj);
    //} else {
      //ASSERT_EQ(OB_HASH_NOT_EXIST, result_entity->get_property(C2, c2_obj));
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, c3_obj));
      //val3.set_varchar(c3_new_value);
      //val3.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      //ASSERT_EQ(val3, c3_obj);
    //}
  //} // end for


  //// get and verify
  //const ObITableEntity *result_entity = NULL;
  //{
    //batch_operation.reset();
    //entity_factory.free_and_reuse();
    //ObObj null_obj;
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //entity = entity_factory.alloc();
      //ASSERT_TRUE(NULL != entity);
      //ObObj key;
      //key.set_int(i*2);
      //ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, null_obj));
      //ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, null_obj));
      //ASSERT_EQ(OB_SUCCESS, batch_operation.retrieve(*entity));
    //}
    //ASSERT_EQ(OB_SUCCESS, table_->batch_execute(batch_operation, result));
    //OB_LOG(INFO, "batch execute result", K(result));
    //ASSERT_EQ(BATCH_SIZE, result.count());
    //for (int64_t i = 0; i < BATCH_SIZE; ++i) {
      //const ObTableOperationResult &r = result.at(i);
      //ASSERT_EQ(OB_SUCCESS, r.get_errno());
      //ASSERT_EQ(ObTableOperationType::GET, r.type());
      //ASSERT_EQ(0, r.get_affected_rows());
      //ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
      //ASSERT_EQ(0, result_entity->get_rowkey_size());
      //ObObj c2_obj, c3_obj;
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, c2_obj));
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, c3_obj));
      ////fprintf(stderr, "%ld (%s,%s)\n", i, S(c2_obj), S(c3_obj));
      //ObObj val2, val3;
      //if (0 == i % 2) {
        //val2.set_int(100+2*i);
        //val3.set_varchar(c3_value);
        //val3.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      //} else {
        //val2.set_int(100+i);
        //val3.set_varchar(c3_new_value);
        //val3.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
      //}
      //ASSERT_EQ(val2, c2_obj);
      //ASSERT_EQ(val3, c3_obj);
    //}
  //}
//}

TEST(TestQueryResult, alloc_memory_if_need)
{
  int ret = OB_SUCCESS;
  const int64_t alloc_size = 1024;
  ObTableQueryResult query_result;
  int64_t total_size = query_result.allocator_.total();
  while (OB_SUCC(ret)) {
    ret = query_result.alloc_buf_if_need(alloc_size);
    query_result.buf_.get_position() += alloc_size;
    if (query_result.allocator_.total() != total_size) {
      total_size = query_result.allocator_.total();
      printf("allocator: %ld, result_buf: %ld\n", query_result.allocator_.total(), query_result.buf_.get_capacity());
    }
  }
  ASSERT_EQ(query_result.buf_.get_capacity(), ObTableQueryResult::MAX_BUF_BLOCK_SIZE * 1);
  ASSERT_GT(query_result.allocator_.total(), ObTableQueryResult::MAX_BUF_BLOCK_SIZE * 1);
  ASSERT_LE(query_result.allocator_.total(), ObTableQueryResult::MAX_BUF_BLOCK_SIZE * 3);
}

//TEST_F(TestBatchExecute, update_table_with_index_by_lowercase_rowkey)
//{

  //OB_LOG(INFO, "begin update_table_with_index_by_lowercase_rowkey");
  //// setup
  //ObTable *the_table = NULL;
  //int ret = service_client_->alloc_table(ObString::make_string("varchar_rowkey_update_test"), the_table);
  //ASSERT_EQ(OB_SUCCESS, ret);
  //// case for insert operation
  //ObTableEntityFactory<ObTableEntity> entity_factory;
  //ObITableEntity *insert_entity = NULL;
  //ObITableEntity *update_entity = NULL;
  //ObTableOperationResult r;
  //{
    //// case: insert with rowkey
    //insert_entity = entity_factory.alloc();
    //ASSERT_TRUE(NULL != insert_entity);

    //const char * rk_value = "TEST";
    //int64_t v_value = 139107;
    //ObObj rk_obj;
    //rk_obj.set_varchar(rk_value);
    //rk_obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ObObj v_obj;
    //v_obj.set_int(v_value);
    //ASSERT_EQ(OB_SUCCESS, insert_entity->add_rowkey_value(rk_obj));
    //ASSERT_EQ(OB_SUCCESS, insert_entity->set_property(T, v_obj));
    //ObTableOperation table_operation = ObTableOperation::insert(*insert_entity);
    //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //}

  //{
    //// case: update with lowercase rowkey
    //update_entity = entity_factory.alloc();
    //ASSERT_TRUE(NULL != update_entity);

    //const char * rk_value = "test";
    //int64_t v_value = 1;
    //ObObj rk_obj;
    //rk_obj.set_varchar(rk_value);
    //rk_obj.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    //ObObj v_obj;
    //v_obj.set_int(v_value);
    //ASSERT_EQ(OB_SUCCESS, update_entity->add_rowkey_value(rk_obj));
    //ASSERT_EQ(OB_SUCCESS, update_entity->set_property(T, v_obj));
    //ObTableOperation table_operation = ObTableOperation::insert_or_update(*update_entity);
    //ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    //ASSERT_EQ(OB_SUCCESS, r.get_errno());
  //}
  //{
    //// query with index
    //ObTableQuery query;
    //ASSERT_EQ(OB_SUCCESS, query.add_select_column(K));
    //ObNewRange range;
    //range.set_whole_range();
    //ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    //query.set_scan_index(ObString::make_string("idx_T"));
    //ObTableEntityIterator *iter = nullptr;
    //ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    //const ObITableEntity *result_entity = NULL;
    //ObObj obj1, obj2, obj3;
    //ObString str;
    //for (int64_t i = 0; i < 1; ++i)
    //{
      //ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
      //ASSERT_EQ(1, result_entity->get_properties_count());
      //ASSERT_EQ(OB_SUCCESS, result_entity->get_property(K, obj1));
      //ASSERT_EQ(OB_SUCCESS, obj1.get_varchar(str));
      //ASSERT_TRUE(str == ObString::make_string("test"));
    //}
    //ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
  //}

  //// todo@wenqu: mysqlproxy使用时报-1044，后面再调通。
////  ObISQLClient::ReadResult res;
////  uint64_t tenant_id = service_client_->get_tenant_id();
////  ObString col_val;
////  ret = service_client_->get_user_sql_client().read(res, tenant_id,
////      "select /*+ index(varchar_rowkey_update_test idx_T)*/ K from test.varchar_rowkey_update_test");
////  ASSERT_EQ(OB_SUCCESS, ret) << "tenant_id: " << tenant_id << "\n";
////  sqlclient::ObMySQLResult *mysql_result = res.get_result();
////  ASSERT_TRUE(NULL != mysql_result);
////  ASSERT_EQ(OB_SUCCESS, mysql_result->next());
////  ASSERT_EQ(OB_SUCCESS, mysql_result->get_varchar("K", col_val));
////  ASSERT_TRUE(col_val == ObString::make_string("TEST"));

  //////////////////////////////////////////////////////////////////
  //// teardown
  //service_client_->free_table(the_table);
  //the_table = NULL;
//}

// CREATE TABLE IF NOT EXISTS `kv_query_test` (
//  C1 bigint,
//  C2 bigint,
//  C3 bigint,
//  PRIMARY KEY(`C1`, `C2`),
//  KEY idx_c2 (`C2`),
//  KEY idx_c3 (`C3`),
//  KEY idx_c2c3(`C2`, `C3`));
TEST_F(TestBatchExecute, table_query_with_secondary_index)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("kv_query_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableOperationResult r;
  ObITableEntity *entity = NULL;
  const ObITableEntity *result_entity = NULL;
  ObTableOperation table_operation;
  ObTableEntityIterator *iter = nullptr;

  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  int64_t key_key = 1;
  ObObj key;
  key.set_int(key_key);
  int64_t value_value = 1;
  ObObj value;
  value.set_int(value_value);


  //prepare data
  const int64_t batch_size = 100;
  for (int64_t i = 0; i < batch_size; ++i) {
    entity->reset();
    key.set_int(key_key + i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));

    value.set_int(value_value);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));

    table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  }
  entity->reset();

  //scan
  ObTableQuery query;

  {
    // case 1: primary key
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start[2];
    pk_objs_start[0].set_int(0);
    pk_objs_start[1].set_min_value();
    ObObj pk_objs_end[2];
    pk_objs_end[0].set_max_value();
    pk_objs_end[1].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 2);
    range.end_key_.assign(pk_objs_end, 2);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("primary")));
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    int64_t result_cnt = 0;
    while (OB_SUCC(iter->get_next_entity(result_entity))) {
      result_cnt++;
    }
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  {
    // case 2: index is the subset of primary key
    query.reset();

    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start;
    pk_objs_start.set_int(0);
    ObObj pk_objs_end;
    pk_objs_end.set_max_value();
    ObNewRange range;
    range.start_key_.assign(&pk_objs_start, 1);
    range.end_key_.assign(&pk_objs_end, 1);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("idx_c2")));
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    int64_t result_cnt = 0;
    while (OB_SUCC(iter->get_next_entity(result_entity))) {
      result_cnt++;
    }
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  {
    // case 3: has itersection between primary key and index
    query.reset();

    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start[2];
    pk_objs_start[0].set_int(0);
    pk_objs_start[1].set_min_value();
    ObObj pk_objs_end[2];
    pk_objs_end[0].set_max_value();
    pk_objs_end[1].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 2);
    range.end_key_.assign(pk_objs_end, 2);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("idx_c2c3")));
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    int64_t result_cnt = 0;
    while (OB_SUCC(iter->get_next_entity(result_entity))) {
      result_cnt++;
    }
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  {
    // case 4: has no itersection between primary key and index
    query.reset();

    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start;
    pk_objs_start.set_int(0);
    ObObj pk_objs_end;
    pk_objs_end.set_max_value();
    ObNewRange range;
    range.start_key_.assign(&pk_objs_start, 1);
    range.end_key_.assign(&pk_objs_end, 1);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("idx_c3")));
    ASSERT_EQ(OB_SUCCESS, the_table->execute_query(query, iter));
    int64_t result_cnt = 0;
    while (OB_SUCC(iter->get_next_entity(result_entity))) {
      result_cnt++;
    }
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  // teardown
  iter = nullptr;
  service_client_->free_table(the_table);
  the_table = NULL;
}

// create table if not exists query_sync_multi_batch_test (PK1 bigint, PK2 bigint, C1 bigint, C2 varchar(100), C3 bigint, PRIMARY KEY(PK1, PK2), INDEX idx1(C1, C2));
/*TEST_F(TestBatchExecute, query_sync_multi_batch)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("query_sync_multi_batch_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableBatchOperation batch_operation;
  ObITableEntity *entity = NULL;
  for (int64_t i = 0; i < BATCH_SIZE; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(i%5);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    key.set_int(i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ObObj value;
    value.set_int(100+i);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C1, value));
    value.set_varchar(ObString::make_string("c2_value"));
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
  }
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  printf("insert data into query_sync_multi_batch_test using batch_execute...\n");
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  ASSERT_EQ(BATCH_SIZE, result.count());
  for (int64_t i = 0; i < BATCH_SIZE; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for

  // cases
  printf("begin to execute query...\n");
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(PK2));
  ObObj pk_objs_start[2];
  pk_objs_start[0].set_int(0);
  pk_objs_start[1].set_min_value();
  ObObj pk_objs_end[2];
  pk_objs_end[0].set_int(0);
  pk_objs_end[1].set_max_value();
  ObNewRange range;
  range.start_key_.assign(pk_objs_start, 2);
  range.end_key_.assign(pk_objs_end, 2);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
  query.set_offset(5);
  query.set_limit(10);
  query.set_batch(1);
  query.set_scan_index(ObString::make_string("primary"));
  ObQueryFlag::ScanOrder scan_orders[2] = { ObQueryFlag::Forward, ObQueryFlag::Reverse };
  for (int k = 0; k < 2; ++k)
  {
    // two scan order
    query.set_scan_order(scan_orders[k]);
    ObTableQuerySyncResult *iter = nullptr;
    ASSERT_EQ(OB_SUCCESS, the_table->query_start(query, iter));
    const ObITableEntity *result_entity = NULL;
    int result_cnt = 0;
    for (int64_t i = 5; i < 15; ++i)
    {
      printf("start to get entity from result iterator, i: %ld, result_cnt: %d\n", i, result_cnt);
      ASSERT_EQ(1, iter->get_row_count());
      ASSERT_EQ(OB_SUCCESS, iter->get_next_entity(result_entity));
      ASSERT_EQ(4, result_entity->get_properties_count());

      ObObj value;
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C1, value));
      if (0 == k) {
        ASSERT_EQ(100+i*5, value.get_int());
      } else {
        ASSERT_EQ(100+(19-i)*5, value.get_int());
      }
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C2, value));
      ObString str;
      ASSERT_EQ(OB_SUCCESS, value.get_varchar(str));
      ASSERT_TRUE(str == ObString::make_string("c2_value"));
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(C3, value));
      ASSERT_TRUE(value.is_null());
      ASSERT_EQ(OB_SUCCESS, result_entity->get_property(PK2, value));
      if (0 == k) {
        ASSERT_EQ(i*5, value.get_int());
      } else {
        ASSERT_EQ((19-i)*5, value.get_int());
      }
      ++result_cnt;
      ASSERT_EQ(OB_ITER_END, iter->get_next_entity(result_entity));
      if (i == 14) {
        ASSERT_EQ(OB_ITER_END, the_table->query_next(iter));
        ASSERT_EQ(0, iter->get_row_count());
      } else {
        ASSERT_EQ(OB_SUCCESS, the_table->query_next(iter));
      }
    }
    ASSERT_EQ(query.get_limit(), result_cnt);
  } // end for

  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

// create table if not exists large_scan_query_sync_test (C1 bigint primary key, C2 bigint, C3 varchar(100));
TEST_F(TestBatchExecute, large_scan_query_sync)
{
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("large_scan_query_sync_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableOperationResult r;
  ObITableEntity *entity = NULL;
  const ObITableEntity *result_entity = NULL;
  ObTableOperation table_operation;

  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  int64_t key_key = 139107;
  ObObj key;
  key.set_int(key_key);
  int64_t value_value = 33521;
  ObObj value;
  value.set_int(value_value);

  //prepare data
  const int64_t large_batch_size = 10000;
  const int64_t query_round = 4;
  ObString c3_value = ObString::make_string("hello world");
  for (int64_t i = 0; i < large_batch_size; ++i) {
    entity->reset();
    key.set_int(key_key + i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    value.set_int(value_value);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    value.set_varchar(c3_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  }
  entity->reset();

  // forward scan
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
  ObObj pk_objs_start;
  pk_objs_start.set_int(0);
  ObObj pk_objs_end;
  pk_objs_end.set_max_value();
  ObNewRange range;
  range.start_key_.assign(&pk_objs_start, 1);
  range.end_key_.assign(&pk_objs_end, 1);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
  query.set_scan_index(ObString::make_string("primary"));
  query.set_scan_order(ObQueryFlag::Forward);
  query.set_batch(large_batch_size / query_round);

  ObTableQuerySyncResult *iter = nullptr;
  uint64_t result_cnt = 0;

  ASSERT_EQ(OB_SUCCESS, the_table->query_start(query, iter));
  do {
    printf("iterator row count: %ld\n", iter->get_row_count());
    while (OB_SUCC(iter->get_next_entity(result_entity))) {
      ++result_cnt;
    }
    ASSERT_EQ(OB_ITER_END, ret);
  } while (OB_SUCC(the_table->query_next(iter)));
  ASSERT_EQ(OB_ITER_END, ret);
  ASSERT_EQ(large_batch_size, result_cnt);

  //reverse scan
  query.set_scan_order(ObQueryFlag::Reverse);
  ASSERT_EQ(OB_SUCCESS, the_table->query_start(query, iter));
  result_cnt = 0;
  do {
    printf("iterator row count: %ld\n", iter->get_row_count());
    while (OB_SUCC(iter->get_next_entity(result_entity))) {
      ++result_cnt;
    }
  } while (OB_SUCC(the_table->query_next(iter)));
  ASSERT_EQ(OB_ITER_END, ret);
  ASSERT_EQ(result_cnt, large_batch_size);

  // teardown
  service_client_->free_table(the_table);
  the_table = NULL;
}

// create table if not exists query_sync_with_index_test (C1 bigint, C2 bigint, C3 bigint, primary key(C1, C2), KEY idx_c2 (C2), KEY idx_c3 (C3), KEY idx_c2c3(C2, C3));
TEST_F(TestBatchExecute, query_sync_with_index)
{
  // setup
  ObTable *the_table = NULL;
  int ret = service_client_->alloc_table(ObString::make_string("query_sync_with_index_test"), the_table);
  ASSERT_EQ(OB_SUCCESS, ret);
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableOperationResult r;
  ObITableEntity *entity = NULL;
  const ObITableEntity *result_entity = NULL;
  ObTableOperation table_operation;
  ObTableQuerySyncResult *iter = nullptr;

  entity = entity_factory.alloc();
  ASSERT_TRUE(NULL != entity);
  int64_t key_key = 1;
  ObObj key;
  key.set_int(key_key);
  int64_t value_value = 1;
  ObObj value;
  value.set_int(value_value);


  //prepare data
  const int64_t batch_size = 100;
  const int64_t query_round = 5;
  for (int64_t i = 0; i < batch_size; ++i) {
    entity->reset();
    key.set_int(key_key + i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));

    value.set_int(value_value);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));

    table_operation = ObTableOperation::insert(*entity);
    ASSERT_EQ(OB_SUCCESS, the_table->execute(table_operation, r));
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
  }
  entity->reset();

  //scan
  ObTableQuery query;
  {
    // case 1: primary key
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start[2];
    pk_objs_start[0].set_int(0);
    pk_objs_start[1].set_min_value();
    ObObj pk_objs_end[2];
    pk_objs_end[0].set_max_value();
    pk_objs_end[1].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 2);
    range.end_key_.assign(pk_objs_end, 2);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("primary")));
    ASSERT_EQ(OB_SUCCESS, the_table->query_start(query, iter));
    ASSERT_EQ(OB_SUCCESS, query.set_batch(batch_size / query_round));
    int64_t result_cnt = 0;
    do {
      while (OB_SUCC(iter->get_next_entity(result_entity))) {
        ++result_cnt;
      }
      ASSERT_EQ(OB_ITER_END, ret);
    } while (OB_SUCC(the_table->query_next(iter)));
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  {
    // case 2: index is the subset of primary key
    query.reset();

    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start;
    pk_objs_start.set_int(0);
    ObObj pk_objs_end;
    pk_objs_end.set_max_value();
    ObNewRange range;
    range.start_key_.assign(&pk_objs_start, 1);
    range.end_key_.assign(&pk_objs_end, 1);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("idx_c2")));
    ASSERT_EQ(OB_SUCCESS, the_table->query_start(query, iter));
    ASSERT_EQ(OB_SUCCESS, query.set_batch(batch_size / query_round));
    int64_t result_cnt = 0;
    do {
      while (OB_SUCC(iter->get_next_entity(result_entity))) {
        ++result_cnt;
      }
      ASSERT_EQ(OB_ITER_END, ret);
    } while (OB_SUCC(the_table->query_next(iter)));
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  {
    // case 3: has itersection between primary key and index
    query.reset();

    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start[2];
    pk_objs_start[0].set_int(0);
    pk_objs_start[1].set_min_value();
    ObObj pk_objs_end[2];
    pk_objs_end[0].set_max_value();
    pk_objs_end[1].set_max_value();
    ObNewRange range;
    range.start_key_.assign(pk_objs_start, 2);
    range.end_key_.assign(pk_objs_end, 2);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("idx_c2c3")));
    ASSERT_EQ(OB_SUCCESS, the_table->query_start(query, iter));
    ASSERT_EQ(OB_SUCCESS, query.set_batch(batch_size / query_round));
    int64_t result_cnt = 0;
    do {
      while (OB_SUCC(iter->get_next_entity(result_entity))) {
        ++result_cnt;
      }
      ASSERT_EQ(OB_ITER_END, ret);
    } while (OB_SUCC(the_table->query_next(iter)));
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  {
    // case 4: has no itersection between primary key and index
    query.reset();

    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
    ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
    ObObj pk_objs_start;
    pk_objs_start.set_int(0);
    ObObj pk_objs_end;
    pk_objs_end.set_max_value();
    ObNewRange range;
    range.start_key_.assign(&pk_objs_start, 1);
    range.end_key_.assign(&pk_objs_end, 1);
    range.border_flag_.set_inclusive_start();
    range.border_flag_.set_inclusive_end();

    ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
    ASSERT_EQ(OB_SUCCESS, query.set_scan_index(ObString::make_string("idx_c3")));
    ASSERT_EQ(OB_SUCCESS, the_table->query_start(query, iter));
    ASSERT_EQ(OB_SUCCESS, query.set_batch(batch_size / query_round));
    int64_t result_cnt = 0;
    do {
      while (OB_SUCC(iter->get_next_entity(result_entity))) {
        ++result_cnt;
      }
      ASSERT_EQ(OB_ITER_END, ret);
    } while (OB_SUCC(the_table->query_next(iter)));
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(result_cnt, batch_size);
  }

  // teardown
  iter = nullptr;
  service_client_->free_table(the_table);
  the_table = NULL;
}

// create table if not exists query_sync_multi_task_test (C1 bigint primary key, C2 bigint, C3 varchar(100));
TEST_F(TestBatchExecute, query_sync_multi_task)
{
  // setup
  constexpr int64_t thread_num = 32;
  const int64_t large_batch_size = 100000;
  int64_t query_round = 10;
  const int64_t query_batch_size = large_batch_size / query_round;
  ObVector<ObTable *> tables;

  printf("create client table handle\n");
  ObTable *the_table;
  ObTableRequestOptions request_options;
  request_options.set_returning_affected_rows(true);
  request_options.set_server_timeout(120*1000*1000); // 120s
  for (int64_t i = 0; i < thread_num; ++i) {
    the_table =  NULL;
    int ret = service_client_->alloc_table(ObString::make_string("query_sync_multi_task_test"), the_table);
    OB_LOG(INFO, "alloc_table succeed", K(i));
    ASSERT_EQ(OB_SUCCESS, ret);
    the_table->set_default_request_options(request_options);
    tables.push_back(the_table);
  }

  printf("prepare data\n");
  ObTableEntityFactory<ObTableEntity> entity_factory;
  ObTableOperationResult r;
  ObITableEntity *entity = NULL;
  ObTableBatchOperation batch_operation;
  int64_t key_key = 139107;
  ObObj key;
  key.set_int(key_key);
  int64_t value_value = 33521;
  ObObj value;
  value.set_int(value_value);

  // prepare data
  ObString c3_value = ObString::make_string("hello world");
  clock_t start = clock();
  for (int64_t i = 0; i < large_batch_size; ++i) {
    entity = entity_factory.alloc();
    ASSERT_TRUE(NULL != entity);
    ObObj key;
    key.set_int(key_key + i);
    ASSERT_EQ(OB_SUCCESS, entity->add_rowkey_value(key));
    ObObj value;
    value.set_int(value_value);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C2, value));
    value.set_varchar(c3_value);
    value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ASSERT_EQ(OB_SUCCESS, entity->set_property(C3, value));
    ASSERT_EQ(OB_SUCCESS, batch_operation.insert(*entity));
  }
  printf("time elapsed during data preparation: %lfs\n", double(clock() - start) / CLOCKS_PER_SEC);
  start = clock();
  ASSERT_TRUE(!batch_operation.is_readonly());
  ASSERT_TRUE(batch_operation.is_same_type());
  ASSERT_TRUE(batch_operation.is_same_properties_names());
  ObTableBatchOperationResult result;
  ASSERT_EQ(OB_SUCCESS, the_table->batch_execute(batch_operation, result));
  ASSERT_EQ(large_batch_size, result.count());
  printf("time elapsed during batch_execute: %lfs\n", double(clock() - start) / CLOCKS_PER_SEC);

  start = clock();
  for (int64_t i = 0; i < large_batch_size; ++i)
  {
    const ObTableOperationResult &r = result.at(i);
    ASSERT_EQ(OB_SUCCESS, r.get_errno());
    ASSERT_EQ(1, r.get_affected_rows());
    ASSERT_EQ(ObTableOperationType::INSERT, r.type());
    const ObITableEntity *result_entity = NULL;
    ASSERT_EQ(OB_SUCCESS, r.get_entity(result_entity));
    ASSERT_TRUE(result_entity->is_empty());
  } // end for
  printf("time elapsed during batch_execute result check: %lfs\n", double(clock() - start) / CLOCKS_PER_SEC);

  // forward scan
  ObTableQuery query;
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C1));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C2));
  ASSERT_EQ(OB_SUCCESS, query.add_select_column(C3));
  ObObj pk_objs_start;
  pk_objs_start.set_int(0);
  ObObj pk_objs_end;
  pk_objs_end.set_max_value();
  ObNewRange range;
  range.start_key_.assign(&pk_objs_start, 1);
  range.end_key_.assign(&pk_objs_end, 1);
  range.border_flag_.set_inclusive_start();
  range.border_flag_.set_inclusive_end();

  ASSERT_EQ(OB_SUCCESS, query.add_scan_range(range));
  query.set_scan_index(ObString::make_string("primary"));
  query.set_batch(query_batch_size); // ensure multiple threads handle one query
  query.set_scan_order(ObQueryFlag::Forward);

  auto task = [&](ObTable * one_table) {
    int ret;
    ObTableQuerySyncResult *iter = nullptr;
    uint64_t result_cnt = 0;
    ObObj one_value;
    const ObITableEntity *one_result_entity = NULL;

    ASSERT_EQ(OB_SUCCESS, one_table->query_start(query, iter));
    do {
      OB_LOG(INFO, "start to do query sync task", K(iter->get_row_count()));
      while (OB_SUCC(iter->get_next_entity(one_result_entity))) {
        ASSERT_EQ(OB_SUCCESS, one_result_entity->get_property(C1, one_value));
        ASSERT_EQ(key_key + result_cnt, one_value.get_int());
        ASSERT_EQ(OB_SUCCESS, one_result_entity->get_property(C2, one_value));
        ASSERT_EQ(value_value, one_value.get_int());
        ASSERT_EQ(OB_SUCCESS, one_result_entity->get_property(C3, one_value));
        ObString str;
        ASSERT_EQ(OB_SUCCESS, one_value.get_varchar(str));
        ASSERT_EQ(c3_value, str);
          ++result_cnt;
      }
    } while (OB_SUCC(one_table->query_next(iter)));
    ASSERT_EQ(OB_ITER_END, ret);
    ASSERT_EQ(large_batch_size, result_cnt);

    OB_LOG(INFO, "finsh query sync task", K(result_cnt));
  };

  std::vector<std::thread> threads;
  uint64_t N = tables.size();
  ASSERT_EQ(thread_num, N);

  time_t start_time = time(NULL);
  printf("begin to run tasks, thread_num: %ld, large_batch_size: %ld, query_round: %ld\n", thread_num, large_batch_size, query_round);
  for (uint64_t i = 0; i < N; ++i) {
    std::thread t(task, tables.at(i));
    threads.push_back(std::move(t));
  }
  for (uint64_t i = 0; i < N; ++i) {
    threads.at(i).join();
  }
  printf("time elapsed during query process: %lfs\n", double(time(NULL) - start_time));

  // teardown
  for (uint64_t i = 0; i < N; ++i) {
    service_client_->free_table(tables.at(i));
  }
}*/


int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc,argv);
  if (argc == 9)
  {
    host = argv[1];
    sql_port = atoi(argv[2]);
    tenant = argv[3];
    user_name = argv[4];
    passwd = argv[5];
    db = argv[6];
    table_name = argv[7];
    rpc_port = atoi(argv[8]);
  }
  ObTableServiceLibrary::init();
  int ret = RUN_ALL_TESTS();
  ObTableServiceLibrary::destroy();
  return ret;
}
