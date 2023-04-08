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

#define USING_LOG_PREFIX SHARE

#include "ob_ls_status_operator.h"

#include "share/ob_errno.h"
#include "share/config/ob_server_config.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "lib/string/ob_sql_string.h"
#include "common/ob_timeout_ctx.h"
#include "share/ob_share_util.h"
#include "lib/mysqlclient/ob_mysql_transaction.h"
#include "share/ls/ob_ls_log_stat_info.h" // ObLSLogStatInfo
#include "rootserver/ob_server_manager.h" // ObServerManager
#include "rootserver/ob_zone_manager.h" // ObZoneManager
#include "rootserver/ob_root_utils.h" // majority
#include "logservice/palf/log_define.h" // INVALID_PROPOSAL_ID
#include "share/schema/ob_multi_version_schema_service.h" // ObMultiVersionSchemaService
#include "share/scn.h" // SCN

using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::rootserver;
using namespace oceanbase::palf;
namespace oceanbase
{
namespace share
{
//////////ObLSStatusInfo
bool ObLSStatusInfo::is_valid() const
{
  return ls_id_.is_valid()
         && OB_INVALID_TENANT_ID != tenant_id_
         && (ls_id_.is_sys_ls()
             || (OB_INVALID_ID != ls_group_id_
                 && OB_INVALID_ID != unit_group_id_))
         && share::OB_LS_EMPTY != status_;
}

void ObLSStatusInfo::reset()
{
  tenant_id_ = OB_INVALID_TENANT_ID;
  ls_id_.reset();
  ls_group_id_ = OB_INVALID_ID;
  unit_group_id_ = OB_INVALID_ID;
  status_ = OB_LS_EMPTY;
}

int ObLSStatusInfo::init(const uint64_t tenant_id,
                         const ObLSID &id,
                         const uint64_t ls_group_id,
                         const ObLSStatus status,
                         const uint64_t unit_group_id,
                         const ObZone &primary_zone)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!id.is_valid()
                  || OB_INVALID_TENANT_ID == tenant_id
                  || OB_LS_EMPTY == status)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(id), K(ls_group_id),
              K(status), K(unit_group_id));
  } else if (OB_FAIL(primary_zone_.assign(primary_zone))) {
    LOG_WARN("failed to assign primary zone", KR(ret), K(primary_zone));
  } else {
    tenant_id_ = tenant_id;
    ls_id_ = id;
    ls_group_id_ = ls_group_id;
    unit_group_id_ = unit_group_id;
    status_ = status;
  }
  return ret;
}

int ObLSStatusInfo::assign(const ObLSStatusInfo &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    if (OB_FAIL(primary_zone_.assign(other.primary_zone_))) {
      LOG_WARN("failed to assign other primary zone", KR(ret), K(other));
    } else {
      tenant_id_ = other.tenant_id_;
      ls_id_ = other.ls_id_;
      ls_group_id_ = other.ls_group_id_;
      unit_group_id_ = other.unit_group_id_;
      status_ = other.status_;
      unit_group_id_ = other.unit_group_id_;
    }
  }
  return ret;
}

bool ls_is_empty_status(const ObLSStatus &status)
{
  return OB_LS_EMPTY == status;
}

bool ls_is_creating_status(const ObLSStatus &status)
{
  return OB_LS_CREATING == status;
}

bool ls_is_created_status(const ObLSStatus &status)
{
  return OB_LS_CREATED == status;
}

bool ls_is_normal_status(const ObLSStatus &status)
{
  return OB_LS_NORMAL == status;
}

bool ls_is_tenant_dropping_status(const ObLSStatus &status)
{
  return OB_LS_TENANT_DROPPING == status;
}

bool ls_is_dropping_status(const ObLSStatus &status)
{
  return OB_LS_DROPPING == status;
}

bool ls_is_wait_offline_status(const ObLSStatus &status)
{
  return OB_LS_WAIT_OFFLINE == status;
}
bool ls_is_create_abort_status(const ObLSStatus &status)
{
  return OB_LS_CREATE_ABORT == status;
}

bool ls_need_create_abort_status(const ObLSStatus &status)
{
  return OB_LS_CREATING == status || OB_LS_CREATED == status;
}

bool ls_is_pre_tenant_dropping_status(const ObLSStatus &status)
{
  return OB_LS_PRE_TENANT_DROPPING == status;
}


bool is_valid_status_in_ls(const ObLSStatus &status)
{
  return OB_LS_CREATING == status || OB_LS_NORMAL == status
         || OB_LS_DROPPING == status || OB_LS_TENANT_DROPPING == status
         || OB_LS_PRE_TENANT_DROPPING == status;
}


/////////ObLSPrimaryZoneInfo
int ObLSPrimaryZoneInfo::init(const uint64_t tenant_id, const uint64_t ls_group_id, const ObLSID ls_id,
           const ObZone &primary_zone, const ObString &zone_priority)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!ls_id.is_valid()
                  || OB_INVALID_TENANT_ID == tenant_id
                  || OB_INVALID_ID == ls_group_id
                  || primary_zone.is_empty()
                  || zone_priority.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(ls_id), K(tenant_id),
              K(primary_zone), K(zone_priority), K(ls_group_id));
  } else if (OB_FAIL(primary_zone_.assign(primary_zone))) {
    LOG_WARN("failed to assign primary zone", KR(ret), K(primary_zone));
  } else if (OB_FAIL(zone_priority_.assign(zone_priority))) {
    LOG_WARN("failed to assign normalize primary zone", KR(ret), K(zone_priority));
  } else {
    tenant_id_ = tenant_id;
    ls_group_id_ = ls_group_id;
    ls_id_ = ls_id;
  }

  return ret;
}

int ObLSPrimaryZoneInfo::assign(const ObLSPrimaryZoneInfo &other)
{
  int ret = OB_SUCCESS;
  if (this != &other) {
    if (OB_FAIL(primary_zone_.assign(other.primary_zone_))) {
      LOG_WARN("failed to assign other primary zone", KR(ret), K(other));
    } else if (OB_FAIL(zone_priority_.assign(other.zone_priority_))) {
      LOG_WARN("failed to assign normalize primary zone", KR(ret), K(other));
    } else {
      tenant_id_ = other.tenant_id_;
      ls_group_id_ = other.ls_group_id_;
      ls_id_ = other.ls_id_;
    }
  }
  return ret;

}

////////ObLSStatusOperator
const char* ObLSStatusOperator::LS_STATUS_ARRAY[] =
{
  "CREATING",
  "CREATED",
  "NORMAL",
  "DROPPING",
  "TENANT_DROPPING",
  "WAIT_OFFLINE",
  "CREATE_ABORT",
  "PRE_TENANT_DROPPING"
};

ObLSStatus ObLSStatusOperator::str_to_ls_status(const ObString &status_str)
{
  ObLSStatus ret_status = OB_LS_EMPTY;
  if (status_str.empty()) {
    ret_status = OB_LS_EMPTY;
  } else {
    for (int64_t i = 0; i < ARRAYSIZEOF(LS_STATUS_ARRAY); i++) {
      if (0 == status_str.case_compare(LS_STATUS_ARRAY[i])) {
        ret_status = static_cast<ObLSStatus>(i);
        break;
      }
    }
  }
  return ret_status;
}

const char* ObLSStatusOperator::ls_status_to_str(const ObLSStatus &status)
{
  const char* str = "UNKNOWN";

  if (OB_UNLIKELY(OB_LS_EMPTY == status)) {
    LOG_WARN_RET(OB_INVALID_ARGUMENT, "invalid log stream status", K(status));
  } else {
    str = LS_STATUS_ARRAY[status];
  }
  return str;
}

int ObLSStatusOperator::create_new_ls(const ObLSStatusInfo &ls_info,
                                      const SCN &current_tenant_scn,
                                      const common::ObString &zone_priority,
                                      const share::ObTenantSwitchoverStatus &working_sw_status,
                                      ObMySQLTransaction &trans)
{
  UNUSEDx(current_tenant_scn, zone_priority);
  int ret = OB_SUCCESS;
  ObAllTenantInfo tenant_info;
  if (OB_UNLIKELY(!ls_info.is_valid()
                  || !working_sw_status.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(ls_info), K(working_sw_status));
  } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(
                  ls_info.tenant_id_, &trans, true, tenant_info))) {
    LOG_WARN("failed to load tenant info", KR(ret), K(ls_info));
  } else if (working_sw_status != tenant_info.get_switchover_status()) {
    ret = OB_NEED_RETRY;
    LOG_WARN("tenant not in specified switchover status", K(ls_info), K(working_sw_status), K(tenant_info));
  } else {
    common::ObSqlString sql;
    if (OB_FAIL(sql.assign_fmt("INSERT into %s (tenant_id, ls_id, status, "
                           "ls_group_id, unit_group_id, primary_zone) "
                           "values (%lu, %ld, '%s', %ld, %ld, '%s')",
                           OB_ALL_LS_STATUS_TNAME, ls_info.tenant_id_, ls_info.ls_id_.id(),
                           ls_status_to_str(ls_info.status_),
                           ls_info.ls_group_id_, ls_info.unit_group_id_,
                           ls_info.primary_zone_.ptr()))) {
      LOG_WARN("failed to assing sql", KR(ret), K(ls_info));
    } else if (OB_FAIL(exec_write(ls_info.tenant_id_, sql, this, trans))) {
      LOG_WARN("failed to exec write", KR(ret), K(ls_info), K(sql));
    }
  }
  return ret;
}

int ObLSStatusOperator::drop_ls(const uint64_t &tenant_id,
                      const share::ObLSID &ls_id,
                      const ObTenantSwitchoverStatus &working_sw_status,
                      ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  ObAllTenantInfo tenant_info;
  if (OB_UNLIKELY(!ls_id.is_valid() || OB_INVALID_TENANT_ID == tenant_id
                  || !working_sw_status.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(ls_id), K(tenant_id), K(working_sw_status));
  } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(
                     tenant_id, &trans, true, tenant_info))) {
    LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
  } else if (working_sw_status != tenant_info.get_switchover_status()) {
    ret = OB_NEED_RETRY;
    LOG_WARN("tenant not in specified switchover status", K(tenant_id), K(working_sw_status), K(tenant_info));
  } else {
    common::ObSqlString sql;
    if (OB_FAIL(sql.assign_fmt("DELETE from %s where ls_id = %ld and tenant_id = %lu",
                               OB_ALL_LS_STATUS_TNAME, ls_id.id(), tenant_id))) {
      LOG_WARN("failed to assign sql", KR(ret), K(ls_id), K(sql));
    } else if (OB_FAIL(exec_write(tenant_id, sql, this, trans))) {
      LOG_WARN("failed to exec write", KR(ret), K(tenant_id), K(ls_id), K(sql));
    }
  }
  return ret;
}

int ObLSStatusOperator::set_ls_offline(const uint64_t &tenant_id,
                      const share::ObLSID &ls_id,
                      const ObLSStatus &ls_status,
                      const SCN &drop_scn,
                      const ObTenantSwitchoverStatus &working_sw_status,
                      ObMySQLTransaction &trans)
{
  UNUSEDx(drop_scn);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!ls_id.is_valid() || OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(ls_id), K(tenant_id));
  } else if (OB_FAIL(update_ls_status(tenant_id, ls_id,
          ls_status, OB_LS_WAIT_OFFLINE, working_sw_status, trans))) {
    LOG_WARN("failed to update ls status", KR(ret), K(tenant_id), K(ls_id), K(ls_status));
  }
  return ret;
}

int ObLSStatusOperator::update_ls_primary_zone(
      const uint64_t &tenant_id,
      const share::ObLSID &ls_id,
      const common::ObZone &primary_zone,
      const common::ObString &zone_priority,
      ObMySQLTransaction &trans)
{
  UNUSEDx(zone_priority);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!ls_id.is_valid()
                  || primary_zone.is_empty()
                  || OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(ls_id), K(primary_zone), K(tenant_id));
  } else {
    common::ObSqlString sql;
    if (OB_FAIL(sql.assign_fmt("UPDATE %s set primary_zone = '%s' where ls_id "
                               "= %ld and tenant_id = %lu",
                               OB_ALL_LS_STATUS_TNAME, primary_zone.ptr(),
                               ls_id.id(), tenant_id))) {
      LOG_WARN("failed to assign sql", KR(ret), K(ls_id), K(primary_zone), K(sql), K(tenant_id));
    } else if (OB_FAIL(exec_write(tenant_id, sql, this, trans, true/*ignore row*/))) {
      //ls primary zone no need to change, but zone_priority change
      LOG_WARN("failed to exec write", KR(ret), K(ls_id), K(sql), K(tenant_id));
    }
  }
  return ret;
}

int ObLSStatusOperator::update_ls_status(
    const uint64_t tenant_id,
    const ObLSID &id, const ObLSStatus &old_status,
    const ObLSStatus &new_status,
    const ObTenantSwitchoverStatus &switch_status,
    ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!id.is_valid()
                  || OB_LS_EMPTY == new_status
                  || OB_LS_EMPTY == old_status
                  || OB_INVALID_TENANT_ID == tenant_id
                  || !switch_status.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(id), K(new_status), K(old_status),
             K(tenant_id), K(switch_status));
  } else {
    //init_member_list is no need after create success
    ObMySQLTransaction trans;
    ObAllTenantInfo tenant_info;
    const uint64_t exec_tenant_id =
      ObLSLifeIAgent::get_exec_tenant_id(tenant_id);
    if (OB_FAIL(trans.start(&client, exec_tenant_id))) {
      LOG_WARN("failed to start trans", KR(ret), K(exec_tenant_id));
    } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(
                   tenant_id, &trans, true, tenant_info))) {
      LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
    } else if (switch_status != tenant_info.get_switchover_status()) {
      ret = OB_NEED_RETRY;
      LOG_WARN("tenant not expect switchover status", KR(ret), K(tenant_info));
    } else if (OB_FAIL(update_ls_status_in_trans_(tenant_id, id, old_status, new_status, trans))) {
      LOG_WARN("failed to update ls status in trans", KR(ret), K(tenant_id), K(id), K(old_status), K(new_status));
    }
    if (trans.is_started()) {
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
        LOG_WARN("failed to commit trans", KR(ret), KR(tmp_ret));
        ret = OB_SUCC(ret) ? tmp_ret : ret;
      }
    }
  }
  return ret;
}

int ObLSStatusOperator::update_ls_status_in_trans_(
    const uint64_t tenant_id,
    const ObLSID &id, const ObLSStatus &old_status,
    const ObLSStatus &new_status,
    ObMySQLTransaction &trans)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!id.is_valid()
                  || OB_LS_EMPTY == new_status
                  || OB_LS_EMPTY == old_status
                  || OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(id), K(new_status), K(old_status),
             K(tenant_id));
  } else {
    //init_member_list is no need after create success
    common::ObSqlString sql;
    const uint64_t exec_tenant_id =
      ObLSLifeIAgent::get_exec_tenant_id(tenant_id);
    if (OB_FAIL(sql.assign_fmt("UPDATE %s set status = '%s',init_member_list = '', b_init_member_list = ''"
                               " where ls_id = %ld and tenant_id = %lu and status = '%s'",
                               OB_ALL_LS_STATUS_TNAME,
                               ls_status_to_str(new_status), id.id(),
                               tenant_id, ls_status_to_str(old_status)))) {
      LOG_WARN("failed to assign sql", KR(ret), K(id), K(new_status),
               K(old_status), K(tenant_id), K(sql));
    } else if (OB_FAIL(exec_write(tenant_id, sql, this, trans))) {
      LOG_WARN("failed to exec write", KR(ret), K(tenant_id), K(id), K(sql));
    }
  }
  return ret;
}

int ObLSStatusOperator::update_init_member_list(
    const uint64_t tenant_id,
    const ObLSID &id, const ObMemberList &member_list, ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!id.is_valid()
                  || !member_list.is_valid()
                  || OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(id), K(member_list), K(tenant_id));
  } else {
    common::ObSqlString sql;
    ObString visist_member_list;
    ObString hex_member_list;
    ObArenaAllocator allocator("MemberList");
    if (OB_FAIL(get_visible_member_list_str_(member_list, allocator, visist_member_list))) {
      LOG_WARN("failed to get visible member list", KR(ret), K(member_list));
    } else if (OB_FAIL(get_member_list_hex_(member_list, allocator, hex_member_list))) {
      LOG_WARN("faield to get member list hex", KR(ret), K(member_list));
    } else if (OB_FAIL(sql.assign_fmt(
            "UPDATE %s set init_member_list = '%.*s', b_init_member_list = '%.*s' "
            "where ls_id = %ld and tenant_id = %lu and b_init_member_list is null",
            OB_ALL_LS_STATUS_TNAME,
            visist_member_list.length(), visist_member_list.ptr(),
            hex_member_list.length(), hex_member_list.ptr(), id.id(), tenant_id))) {
      LOG_WARN("failed to assign sql", KR(ret), K(id), K(member_list), K(sql));
    } else if (OB_FAIL(exec_write(tenant_id, sql, this, client))) {
      LOG_WARN("failed to exec write", KR(ret), K(id), K(sql));
    }
  }
  return ret;
}

int ObLSStatusOperator::get_all_ls_status_by_order(
    const uint64_t tenant_id,
    ObLSStatusInfoIArray &ls_array, ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  ls_array.reset();
  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("operation is not valid", KR(ret), K(tenant_id));
  } else {
    ObSqlString sql;
    if (OB_FAIL(sql.assign_fmt(
                   "SELECT * FROM %s WHERE tenant_id = %lu ORDER BY tenant_id, ls_id",
                   OB_ALL_LS_STATUS_TNAME, tenant_id))) {
      LOG_WARN("failed to assign sql", KR(ret), K(sql), K(tenant_id));
    } else if (OB_FAIL(exec_read(tenant_id, sql, client, this, ls_array))) {
      LOG_WARN("failed to exec read", KR(ret), K(tenant_id), K(sql));
    }
  }
  return ret;
}

int ObLSStatusOperator::get_all_ls_status_by_order_for_switch_tenant(
    const uint64_t tenant_id,
    const bool ignore_need_create_abort,
    ObLSStatusInfoIArray &ls_array,
    ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  ls_array.reset();
  ObLSStatusInfoArray ori_ls_array;
  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("tenant_id is not valid", KR(ret), K(tenant_id));
  } else if (OB_FAIL(get_all_ls_status_by_order(tenant_id, ori_ls_array, client))) {
    LOG_WARN("failed to get_all_ls_status_by_order", KR(ret), K(tenant_id));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ori_ls_array.count(); ++i) {
      const ObLSStatusInfo &info = ori_ls_array.at(i);
      if (ls_is_pre_tenant_dropping_status(info.get_status()) || ls_is_tenant_dropping_status(info.get_status())) {
        ret = OB_TENANT_HAS_BEEN_DROPPED;
        LOG_WARN("tenant has been dropped", KR(ret), K(info));
      } else if (ls_need_create_abort_status(info.get_status())) {
        if (ignore_need_create_abort) {
          LOG_INFO("ignore ls", KR(ret), K(info));
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected ls status", KR(ret), K(info));
        }
      } else if (ls_is_create_abort_status(info.get_status())) {
        LOG_INFO("ignore ls", KR(ret), K(info));
      } else if (OB_FAIL(ls_array.push_back(info))) {
        LOG_WARN("failed to push_back", KR(ret), K(info), K(ls_array));
      }
    }
  }
  return ret;
}

int ObLSStatusOperator::get_ls_init_member_list(
    const uint64_t tenant_id,
    const ObLSID &id, ObMemberList &member_list,
    share::ObLSStatusInfo &status_info, ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  member_list.reset();
  status_info.reset();
  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("tenant id is invalid", KR(ret), K(tenant_id));
  } else if (OB_FAIL(get_ls_status_(tenant_id, id, true /*need_member_list*/,
                                    member_list, status_info, client))) {
    LOG_WARN("failed to get ls status", KR(ret), K(id), K(tenant_id));
  }
  return ret;
}

int ObLSStatusOperator::get_ls_status_info(
    const uint64_t tenant_id,
  const ObLSID &id, ObLSStatusInfo &status_info, ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  ObMemberList member_list;
  status_info.reset();
  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("tenant id is invalid", KR(ret), K(tenant_id));
  } else if (OB_FAIL(get_ls_status_(tenant_id, id, false /*need_member_list*/,
                                    member_list, status_info, client))) {
    LOG_WARN("failed to get ls status", KR(ret), K(id), K(tenant_id));
  }
  return ret;
}

int ObLSStatusOperator::get_visible_member_list_str_(const ObMemberList &member_list,
                                                    common::ObIAllocator &allocator,
                                                    common::ObString &visible_member_list_str)
{
  int ret = OB_SUCCESS;
  char *member_list_str = NULL;
  const int64_t length = member_list.get_serialize_size();
  int64_t pos = 0;
  if (OB_UNLIKELY(!member_list.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("member list is not valid", KR(ret), K(member_list));
  } else if (OB_UNLIKELY(length > OB_MAX_LONGTEXT_LENGTH + 1)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("member list too long", KR(ret), K(length), K(member_list));
  } else if (OB_ISNULL(member_list_str = static_cast<char *>(allocator.alloc(length)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc buf", KR(ret), K(length));
  } else if (FALSE_IT(pos = member_list.to_string(member_list_str, length))) {
    //nothing
  } else if (OB_UNLIKELY(pos >= length)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("size overflow", KR(ret), K(pos), K(length));
  } else {
    visible_member_list_str.assign(member_list_str, static_cast<int32_t>(pos));
  }
  return ret;
}

int ObLSStatusOperator::get_member_list_hex_(const ObMemberList &member_list,
                                                    common::ObIAllocator &allocator,
                                                    common::ObString &hex_str)
{
  int ret = OB_SUCCESS;
  char *serialize_buf = NULL;
  const int64_t serialize_size = member_list.get_serialize_size();
  int64_t serialize_pos = 0;
  char *hex_buf = NULL;
  const int64_t hex_size = 2 * serialize_size;
  int64_t hex_pos = 0;
  if (OB_UNLIKELY(!member_list.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("member_list is invlaid", KR(ret), K(member_list));
  } else if (OB_UNLIKELY(hex_size > OB_MAX_LONGTEXT_LENGTH + 1)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("format str is too long", KR(ret), K(hex_size), K(member_list));
  } else if (OB_ISNULL(serialize_buf = static_cast<char *>(allocator.alloc(serialize_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc buf", KR(ret), K(serialize_size));
  } else if (OB_FAIL(member_list.serialize(serialize_buf, serialize_size, serialize_pos))) {
    LOG_WARN("failed to serialize set member list arg", KR(ret), K(member_list), K(serialize_size), K(serialize_pos));
  } else if (OB_UNLIKELY(serialize_pos > serialize_size)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("serialize error", KR(ret), K(serialize_pos), K(serialize_size));
  } else if (OB_ISNULL(hex_buf = static_cast<char*>(allocator.alloc(hex_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory", KR(ret), K(hex_size));
  } else if (OB_FAIL(hex_print(serialize_buf, serialize_pos, hex_buf, hex_size, hex_pos))) {
    LOG_WARN("fail to print hex", KR(ret), K(serialize_pos), K(hex_size), K(serialize_buf));
  } else if (OB_UNLIKELY(hex_pos > hex_size)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("encode error", KR(ret), K(hex_pos), K(hex_size));
  } else {
    hex_str.assign_ptr(hex_buf, static_cast<int32_t>(hex_size));
  }
  return ret;
}

int ObLSStatusOperator::set_member_list_with_hex_str_(const common::ObString &str,
                                                                ObMemberList &member_list)
{
  int ret = OB_SUCCESS;
  member_list.reset();
  char *deserialize_buf = NULL;
  const int64_t str_size = str.length();
  const int64_t deserialize_size = str.length() / 2 + 1;
  int64_t deserialize_pos = 0;
  ObArenaAllocator allocator("MemberList");
  if (OB_UNLIKELY(str.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("str is empty", KR(ret));
  } else if (OB_ISNULL(deserialize_buf = static_cast<char*>(allocator.alloc(deserialize_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc memory", KR(ret), K(deserialize_size));
  } else if (OB_FAIL(hex_to_cstr(str.ptr(), str_size, deserialize_buf, deserialize_size))) {
    LOG_WARN("fail to get cstr from hex", KR(ret), K(str_size), K(deserialize_size), K(str));
  } else if (OB_FAIL(member_list.deserialize(deserialize_buf, deserialize_size, deserialize_pos))) {
    LOG_WARN("fail to deserialize set member list arg", KR(ret), K(deserialize_pos), K(deserialize_size),
             K(str));
  } else if (OB_UNLIKELY(deserialize_pos > deserialize_size)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("deserialize error", KR(ret), K(deserialize_pos), K(deserialize_size));
  }
  return ret;

}

int ObLSStatusOperator::fill_cell(
    common::sqlclient::ObMySQLResult *result,
    share::ObLSStatusInfo &status_info)
{
  int ret = OB_SUCCESS;
  status_info.reset();
  if (OB_ISNULL(result)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("result is null", KR(ret));
  } else {
    ObString status_str;
    ObString primary_zone_str;
    int64_t id_value = OB_INVALID_ID;
    uint64_t ls_group_id = OB_INVALID_ID;
    uint64_t unit_group_id = OB_INVALID_ID;
    uint64_t tenant_id = OB_INVALID_TENANT_ID;
    EXTRACT_INT_FIELD_MYSQL(*result, "tenant_id", tenant_id, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result, "ls_id", id_value, int64_t);
    EXTRACT_INT_FIELD_MYSQL(*result, "ls_group_id", ls_group_id, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result, "unit_group_id", unit_group_id, uint64_t);
    EXTRACT_VARCHAR_FIELD_MYSQL(*result, "status", status_str);
    EXTRACT_VARCHAR_FIELD_MYSQL(*result, "primary_zone", primary_zone_str);
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to get result", KR(ret), K(id_value), K(ls_group_id),
               K(unit_group_id), K(status_str), K(primary_zone_str));
    } else {
      ObLSID ls_id(id_value);
      ObZone zone(primary_zone_str);
      if (OB_FAIL(status_info.init(tenant_id, ls_id, ls_group_id,
                               str_to_ls_status(status_str), unit_group_id,
                               zone))) {
        LOG_WARN("failed to init ls operation", KR(ret), K(tenant_id), K(zone),
                 K(ls_group_id), K(ls_id), K(status_str), K(unit_group_id));
      }
    }
  }
  return ret;
}

int ObLSStatusOperator::fill_cell(
    common::sqlclient::ObMySQLResult *result,
    share::ObLSPrimaryZoneInfo &primary_zone_info)
{
  int ret = OB_SUCCESS;
  primary_zone_info.reset();
  if (OB_ISNULL(result)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("result is null", KR(ret));
  } else {
    ObString primary_zone_str;
    ObString normalize_zone_str;
    int64_t id_value = OB_INVALID_ID;
    uint64_t tenant_id = OB_INVALID_TENANT_ID;
    uint64_t ls_group_id = OB_INVALID_ID;
    EXTRACT_INT_FIELD_MYSQL(*result, "tenant_id", tenant_id, uint64_t);
    EXTRACT_INT_FIELD_MYSQL(*result, "ls_id", id_value, int64_t);
    EXTRACT_INT_FIELD_MYSQL(*result, "ls_group_id", ls_group_id, int64_t);
    EXTRACT_VARCHAR_FIELD_MYSQL(*result, "primary_zone", primary_zone_str);
    EXTRACT_VARCHAR_FIELD_MYSQL(*result, "zone_priority", normalize_zone_str);
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to get result", KR(ret), K(id_value), K(ls_group_id),
               K(normalize_zone_str), K(primary_zone_str));
    } else {
      ObLSID ls_id(id_value);
      ObZone zone(primary_zone_str);
      if (OB_FAIL(primary_zone_info.init(tenant_id, ls_group_id, ls_id, zone, normalize_zone_str))) {
        LOG_WARN("failed to init ls operation", KR(ret), K(tenant_id), K(zone),
                 K(ls_id), K(normalize_zone_str));
      }
    }
  }
  return ret;
}


int ObLSStatusOperator::get_ls_status_(const uint64_t tenant_id,
                                       const ObLSID &id,
                                       const bool need_member_list,
                                       ObMemberList &member_list,
                                       share::ObLSStatusInfo &status_info,
                                       ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  member_list.reset();
  status_info.reset();
  if (OB_UNLIKELY(!id.is_valid()
                  || OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(id), K(tenant_id));
  } else {
    ObSqlString sql;
    ObTimeoutCtx ctx;
    const int64_t default_timeout = GCONF.internal_sql_execute_timeout;
    uint64_t exec_tenant_id = get_exec_tenant_id(tenant_id);
    if (OB_UNLIKELY(OB_INVALID_TENANT_ID == exec_tenant_id)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get exec tenant id", KR(ret), K(exec_tenant_id));
    } else if (OB_FAIL(ObShareUtil::set_default_timeout_ctx(ctx, default_timeout))) {
      LOG_WARN("failed to set default timeout ctx", KR(ret), K(default_timeout));
    } else if (OB_FAIL(sql.assign_fmt(
                   "SELECT * FROM %s where ls_id = %ld and tenant_id = %lu",
                   OB_ALL_LS_STATUS_TNAME, id.id(), tenant_id))) {
      LOG_WARN("failed to assign sql", KR(ret), K(sql));
    } else {
      HEAP_VAR(ObMySQLProxy::MySQLResult, res) {
        common::sqlclient::ObMySQLResult *result = NULL;
        if (OB_FAIL(client.read(res, exec_tenant_id, sql.ptr()))) {
          LOG_WARN("failed to read", KR(ret), K(exec_tenant_id), K(sql));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("failed to get sql result", KR(ret));
        } else {
          ObString init_member_list_str;
          ret = result->next();
          if (OB_ITER_END == ret) {
            ret = OB_ENTRY_NOT_EXIST;
          } else if (OB_FAIL(ret)) {
            LOG_WARN("failed to get ls", KR(ret), K(sql));
          } else {
           if (OB_FAIL(fill_cell(result, status_info))) {
              LOG_WARN("failed to construct ls status info", KR(ret));
            } else if (need_member_list) {
              EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(
                  *result, "b_init_member_list", init_member_list_str);
              if (OB_FAIL(ret)) {
                LOG_WARN("failed to get result", KR(ret),
                         K(init_member_list_str));
              } else if (init_member_list_str.empty()) {
                // maybe
              } else if (OB_FAIL(set_member_list_with_hex_str_(
                             init_member_list_str, member_list))) {
                LOG_WARN("failed to set member list", KR(ret),
                         K(init_member_list_str));
              }
            }
          }
          if (OB_SUCC(ret)) {
            if (OB_ITER_END != result->next()) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("expect only one row", KR(ret), K(sql));
            }
          }
        }
      }
    }
  }
  return ret;
}


int ObLSStatusOperator::construct_ls_primary_info_sql_(common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.assign_fmt("select a.tenant_id as tenant_id, a.ls_id as ls_id, "
          "a.primary_zone as primary_zone, a.ls_group_id as ls_group_id, b.zone_priority as zone_priority "
          "from %s as a left join %s b on a.tenant_id = b.tenant_id and a.ls_id = b.ls_id ",
          OB_ALL_LS_STATUS_TNAME, OB_ALL_LS_ELECTION_REFERENCE_INFO_TNAME))) {
    LOG_WARN("failed to assign sql", KR(ret), K(sql));
  }
  return ret;
}

int ObLSStatusOperator::get_ls_primary_zone_info(const uint64_t tenant_id, const ObLSID &ls_id,
                             ObLSPrimaryZoneInfo &primary_zone_info, ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!ls_id.is_valid() || OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(ls_id), K(tenant_id));
  } else {
    common::ObSqlString sql;
    ObSEArray<ObLSPrimaryZoneInfo, 1> ls_primary_zone_array;
    if (OB_FAIL(construct_ls_primary_info_sql_(sql))) {
      LOG_WARN("failed to construct sql", KR(ret), K(sql));
    } else if (OB_FAIL(sql.append_fmt(" where a.ls_id = %ld and a.tenant_id = %lu",
               ls_id.id(), tenant_id))) {
      LOG_WARN("failed to assign sql", KR(ret), K(tenant_id), K(ls_id), K(sql));
    } else if (OB_FAIL(exec_read(tenant_id, sql, client, this, ls_primary_zone_array))) {
      LOG_WARN("failed to read ls recovery", KR(ret), K(tenant_id), K(sql));
    } else if (0 == ls_primary_zone_array.count()) {
      ret = OB_ENTRY_NOT_EXIST;
      LOG_WARN("ls not exist", KR(ret), K(tenant_id), K(ls_id));
    } else if (OB_UNLIKELY(1 != ls_primary_zone_array.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("more than one ls is unexpected", KR(ret), K(ls_primary_zone_array), K(sql));
    } else if (OB_FAIL(primary_zone_info.assign(ls_primary_zone_array.at(0)))) {
      LOG_WARN("failed to assign ls attr", KR(ret), K(ls_primary_zone_array));
    }
  }

  return ret;
}

int ObLSStatusOperator::get_tenant_primary_zone_info_array(const uint64_t tenant_id,
                               ObLSPrimaryZoneInfoIArray &primary_zone_info_array, ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_INVALID_TENANT_ID == tenant_id)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid_argument", KR(ret), K(tenant_id));
  } else {
    common::ObSqlString sql;
    if (OB_FAIL(construct_ls_primary_info_sql_(sql))) {
      LOG_WARN("failed to construct sql", KR(ret), K(sql));
    } else if (OB_FAIL(sql.append_fmt(" where a.tenant_id = %lu order by a.ls_group_id", tenant_id))) {
      LOG_WARN("failed to assign sql", KR(ret), K(tenant_id), K(sql));
    } else if (OB_FAIL(exec_read(tenant_id, sql, client, this, primary_zone_info_array))) {
      LOG_WARN("failed to read ls recovery", KR(ret), K(tenant_id), K(sql));
    }
  }

  return ret;

}

int ObLSStatusOperator::construct_ls_log_stat_info_sql_(common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const char *excluded_status = ls_status_to_str(OB_LS_CREATE_ABORT);
  if (OB_FAIL(sql.assign_fmt(
      "SELECT a.tenant_id, a.ls_id, b.svr_ip, b.svr_port, b.role, b.proposal_id, "
      "b.paxos_member_list, b.paxos_replica_num, b.end_scn "
      "FROM %s AS a LEFT JOIN %s AS b ON a.tenant_id = b.tenant_id AND a.ls_id = b.ls_id "
      "WHERE a.status != '%s' "
      "ORDER BY a.tenant_id, a.ls_id, b.role",
      OB_ALL_VIRTUAL_LS_STATUS_TNAME,
      OB_ALL_VIRTUAL_LOG_STAT_TNAME,
      excluded_status))) {
    LOG_WARN("failed to assign sql", KR(ret), K(sql));
  }
  return ret;
}

int ObLSStatusOperator::check_all_ls_has_majority_and_log_sync(
    const ObZoneManager &zone_mgr,
    const ObServerManager &server_mgr,
    const common::ObIArray<ObAddr> &to_stop_servers,
    const bool skip_log_sync_check,
    const char *print_str,
    schema::ObMultiVersionSchemaService &schema_service,
    ObISQLClient &client,
    bool &need_retry)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(construct_ls_log_stat_info_sql_(sql))) {
    LOG_WARN("failed to construct ls paxos info sql", KR(ret), K(sql));
  } else {
    HEAP_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(client.read(res, OB_SYS_TENANT_ID, sql.ptr()))) {
        LOG_WARN("failed to read", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get sql result", KR(ret), K(sql));
      } else if (OB_FAIL(parse_result_and_check_paxos_(
          *result,
          schema_service,
          zone_mgr,
          server_mgr,
          to_stop_servers,
          skip_log_sync_check,
          print_str,
          need_retry))) {
        LOG_WARN("fail to parse result and check paxos", KR(ret),
            K(to_stop_servers), K(skip_log_sync_check), K(print_str), K(need_retry));
      }
    } // end HEAP_VAR
  }
  return ret;
}

int ObLSStatusOperator::parse_result_and_check_paxos_(
    common::sqlclient::ObMySQLResult &result,
    schema::ObMultiVersionSchemaService &schema_service,
    const ObZoneManager &zone_mgr,
    const ObServerManager &server_mgr,
    const common::ObIArray<ObAddr> &to_stop_servers,
    const bool skip_log_sync_check,
    const char *print_str,
    bool &need_retry)
{
  int ret = OB_SUCCESS;
  ObLSLogStatInfo ls_log_stat_info;
  ObLSLogStatReplica replica;

  while (OB_SUCC(ret)) {
    if (OB_FAIL(result.next())) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("get next result failed", KR(ret));
      }
    }
    // tmp_tenant_id/ls_id are used to print log user error
    uint64_t tmp_tenant_id = OB_INVALID_TENANT_ID;
    int64_t tmp_ls_id = OB_INVALID_ID;
    if (FAILEDx(construct_ls_log_stat_replica_(result, replica, tmp_tenant_id, tmp_ls_id))) {
      if (OB_ERR_NULL_VALUE == ret) {
        need_retry = true;
        char err_msg[MAX_ERROR_LOG_PRINT_SIZE];
        ret = OB_OP_NOT_ALLOW;
        LOG_WARN("fail to get ls log stat info when checking ls_log_stat_info", KR(ret),
            K(tmp_tenant_id), K(tmp_ls_id), K(ls_log_stat_info));
        (void)snprintf(err_msg, sizeof(err_msg), "Tenant(%lu) LS(%ld) has no enough paxos member, %s",
            tmp_tenant_id, tmp_ls_id, print_str);
        LOG_USER_ERROR(OB_OP_NOT_ALLOW, err_msg);
      } else {
        LOG_WARN("fail to construct ls paxos replica", KR(ret), K(tmp_tenant_id), K(tmp_ls_id));
      }
    } else if (OB_UNLIKELY(!replica.is_valid())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid ls log stat info", KR(ret), K(replica));
    } else if (!ls_log_stat_info.is_self_replica(replica)) { // meet next ls_log_stat_info
      // check previous ls_log_stat_info
      if (OB_LIKELY(ls_log_stat_info.is_valid())) {
        if (OB_FAIL(check_ls_log_stat_info_(
            schema_service,
            ls_log_stat_info,
            zone_mgr,
            server_mgr,
            to_stop_servers,
            skip_log_sync_check, 
            print_str,
            need_retry))) {
          LOG_WARN("fail to check ls paxos info", KR(ret), K(ls_log_stat_info));
        }
      }
      // set new ls_log_stat_info
      if (OB_SUCC(ret)) {
        ls_log_stat_info.reset();
        if (OB_FAIL(ls_log_stat_info.init(replica.get_tenant_id(), replica.get_ls_id()))) {
          LOG_WARN("fail to init ls log stat info", KR(ret), K(replica), K(ls_log_stat_info));
        } else if (OB_FAIL(ls_log_stat_info.add_replica(replica))) {
          LOG_WARN("fail to add replica", KR(ret), K(replica));
        }
      }
    } else if (OB_FAIL(ls_log_stat_info.add_replica(replica))) {
      LOG_WARN("fail to add replica", KR(ret), K(replica));
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
    // check the last one
    if (ls_log_stat_info.is_valid()) {
      if (OB_FAIL(check_ls_log_stat_info_(
          schema_service,
          ls_log_stat_info,
          zone_mgr,
          server_mgr,
          to_stop_servers,
          skip_log_sync_check,
          print_str, 
          need_retry))) {
        LOG_WARN("fail to check ls paxos info", KR(ret), K(ls_log_stat_info));
      }
    }
  }
  return ret;
}

// tenant_id and ls_id is used for printing error info
int ObLSStatusOperator::construct_ls_log_stat_replica_(
    const common::sqlclient::ObMySQLResult &result,
    ObLSLogStatReplica &replica,
    uint64_t &tenant_id,
    int64_t &ls_id)
{
  int ret = OB_SUCCESS;
  replica.reset();
  tenant_id = OB_INVALID_TENANT_ID;
  ls_id = ObLSID::INVALID_LS_ID;
  ObRole role = INVALID_ROLE;
  ObString role_str;
  ObAddr server;
  ObString svr_ip;
  int64_t svr_port = OB_INVALID_INDEX;
  int64_t proposal_id = INVALID_PROPOSAL_ID;
  int64_t end_scn = OB_INVALID_SCN_VAL;
  int64_t paxos_replica_num = OB_INVALID_COUNT;
  ObString paxos_member_list_str;
  ObLSReplica::MemberList member_list;

  EXTRACT_INT_FIELD_MYSQL(result, "tenant_id", tenant_id, uint64_t);
  EXTRACT_INT_FIELD_MYSQL(result, "ls_id", ls_id, int64_t);
  
  // if these columns below are NULL, return OB_ERR_NULL_VALUE
  EXTRACT_VARCHAR_FIELD_MYSQL(result, "svr_ip", svr_ip);
  EXTRACT_INT_FIELD_MYSQL(result, "svr_port", svr_port, int64_t);
  EXTRACT_VARCHAR_FIELD_MYSQL(result, "role", role_str);
  EXTRACT_INT_FIELD_MYSQL(result, "proposal_id", proposal_id, int64_t);
  //replica in migrate, member list maybe null
  EXTRACT_VARCHAR_FIELD_MYSQL_SKIP_RET(result, "paxos_member_list", paxos_member_list_str);
  EXTRACT_INT_FIELD_MYSQL(result, "paxos_replica_num", paxos_replica_num, int64_t);
  EXTRACT_UINT_FIELD_MYSQL(result, "end_scn", end_scn, int64_t);

  if (FAILEDx(ObLSReplica::text2member_list(
      to_cstring(paxos_member_list_str), 
      member_list))) {
    LOG_WARN("text2member_list failed", KR(ret), K(paxos_member_list_str));
  } else if (OB_UNLIKELY(!server.set_ip_addr(svr_ip, static_cast<uint32_t>(svr_port)))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to set_ip_addr", KR(ret), K(svr_ip), K(svr_port));
  } else if (OB_FAIL(common::string_to_role(role_str, role))) {
    LOG_WARN("fail to transform string to role", KR(ret), K(role_str));
  } else if (OB_FAIL(replica.init(
      tenant_id,
      ObLSID(ls_id),
      server,
      role,
      proposal_id,
      member_list,
      paxos_replica_num,
      end_scn))) {
    LOG_WARN("fail to init replica", KR(ret), K(tenant_id), K(ls_id), K(server), K(role),
        K(proposal_id), K(member_list), K(paxos_replica_num), K(end_scn));
  }
  LOG_INFO("construct ls log stat replica finished", KR(ret), K(tenant_id), K(ls_id),
      K(svr_ip), K(svr_port), K(role_str), K(proposal_id), K(paxos_member_list_str),
      K(paxos_replica_num), K(end_scn), K(replica));
  return ret;
}

// Check following items:
// 1. check each ls has leader;
// 2. check leader's paxos_replica_num is equal to paxos_replica_num from schema;
// 3. check member_list of each ls has enough valid members;
// 4. check ls_log_stat_info has majority filtered by valid_members;
// 5. (can skip) check each ls' majority is in log sync filtered by valid_members;
int ObLSStatusOperator::check_ls_log_stat_info_(
    schema::ObMultiVersionSchemaService &schema_service,
    const ObLSLogStatInfo &ls_log_stat_info,
    const ObZoneManager &zone_mgr,
    const ObServerManager &server_mgr,
    const common::ObIArray<ObAddr> &to_stop_servers,
    const bool skip_log_sync_check,
    const char *print_str,
    bool &need_retry)
{
  int ret = OB_SUCCESS;
  need_retry = false;
  bool is_passed = true;
  ObArray<ObAddr> valid_servers;
  ObLSLogStatReplica leader;
  char err_msg[MAX_ERROR_LOG_PRINT_SIZE];
  ObSchemaGetterGuard schema_guard;
  const ObTenantSchema *tenant_schema = NULL;
  int64_t paxos_replica_num = OB_INVALID_COUNT;
  int64_t arb_replica_num = 0;

  if (OB_UNLIKELY(!ls_log_stat_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(ls_log_stat_info));
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("fail get schema guard", KR(ret));
  } else if (OB_FAIL(schema_guard.get_tenant_info(ls_log_stat_info.get_tenant_id(), tenant_schema))) {
    LOG_WARN("fail to get tenant info", KR(ret), "tenant_id", ls_log_stat_info.get_tenant_id());
  } else if (OB_ISNULL(tenant_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null tenant_schema ptr", KR(ret));
  } else if (OB_FAIL(tenant_schema->get_paxos_replica_num(schema_guard, paxos_replica_num))) {
    LOG_WARN("failed to get paxos replica num", KR(ret), K(ls_log_stat_info));
  } else if (OB_FAIL(ls_log_stat_info.get_leader_replica(leader))) {
    if (OB_LEADER_NOT_EXIST == ret) {
      need_retry = true;
      ret = OB_OP_NOT_ALLOW;
      LOG_WARN("ls has no leader when checking ls_log_stat_info", KR(ret), K(ls_log_stat_info));
      (void)snprintf(err_msg, sizeof(err_msg), "Tenant(%lu) LS(%ld) has no leader, %s",
          ls_log_stat_info.get_tenant_id(), ls_log_stat_info.get_ls_id().id(), print_str);
      LOG_USER_ERROR(OB_OP_NOT_ALLOW, err_msg);
    } else {
      LOG_WARN("fail to get leader replica", KR(ret), K(ls_log_stat_info));
    }
  } else if (!tenant_schema->get_previous_locality_str().empty()) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("locality is changing, can't stop server or zone",
        KR(ret), K(ls_log_stat_info), K(leader), K(paxos_replica_num), K(to_stop_servers),
        "previous_locality", tenant_schema->get_previous_locality_str());
    (void)snprintf(err_msg, sizeof(err_msg), "Tenant(%lu) locality is changing, %s",
        ls_log_stat_info.get_tenant_id(), print_str);
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, err_msg);
  } else if (leader.get_paxos_replica_num() != paxos_replica_num) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("paxos replica number is incorrect, can't stop server or zone",
        KR(ret), K(ls_log_stat_info), K(leader), K(paxos_replica_num), K(to_stop_servers));
    (void)snprintf(err_msg, sizeof(err_msg), "Tenant(%lu) LS(%ld) paxos replica number does not match with tenant. It should be %ld. %s",
        ls_log_stat_info.get_tenant_id(), leader.get_ls_id().id(), paxos_replica_num, print_str);
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, err_msg);
  } else if (OB_FAIL(generate_valid_servers_(
      leader.get_member_list(),
      zone_mgr,
      server_mgr,
      to_stop_servers,
      valid_servers))) {
    LOG_WARN("fail to generate valid member_list", KR(ret),
        K(to_stop_servers), K(leader), K(ls_log_stat_info));
  } else if (2 == paxos_replica_num
             && OB_FAIL(ObShareUtil::generate_arb_replica_num(
                          ls_log_stat_info.get_tenant_id(),
                          ls_log_stat_info.get_ls_id(),
                          arb_replica_num))) {
    // special case: support stop 1F in 2F1A
    need_retry = true;
    LOG_WARN("fail to generate arb replica num", KR(ret), KPC(tenant_schema), K(ls_log_stat_info));
  } else if (valid_servers.count() + arb_replica_num < rootserver::majority(leader.get_paxos_replica_num())) {
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("ls doesn't have enough valid paxos member when checking ls_log_stat_info",
        KR(ret), K(ls_log_stat_info), K(leader), K(to_stop_servers), K(valid_servers), K(arb_replica_num));
    (void)snprintf(err_msg, sizeof(err_msg), "Tenant(%lu) LS(%ld) has no enough valid paxos member after %s, %s",
        ls_log_stat_info.get_tenant_id(), ls_log_stat_info.get_ls_id().id(), print_str, print_str);
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, err_msg);
  } else if (OB_FAIL(ls_log_stat_info.check_has_majority(valid_servers, arb_replica_num, is_passed))) {
    LOG_WARN("fail to check has majority", KR(ret), K(ls_log_stat_info));
  } else if (!is_passed) {
    need_retry = true; // Query returned data does not match valid_servers
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("can't get enough member by __all_virtual_log_stat",
        KR(ret), K(ls_log_stat_info), K(leader), K(to_stop_servers), K(valid_servers));
    (void)snprintf(err_msg, sizeof(err_msg), "Tenant(%lu) LS(%ld) has no enough valid paxos member after %s, %s",
        ls_log_stat_info.get_tenant_id(), ls_log_stat_info.get_ls_id().id(), print_str, print_str);
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, err_msg);
  } else if (skip_log_sync_check) {
    // skip check_log_in_sync
  } else if (OB_FAIL(ls_log_stat_info.check_log_sync(valid_servers, arb_replica_num, is_passed))) {
    LOG_WARN("fail to check log in sync", KR(ret), K(ls_log_stat_info));
  } else if (!is_passed) {
    need_retry = true;
    ret = OB_OP_NOT_ALLOW;
    LOG_WARN("log not sync when checking ls_log_stat_info",
        KR(ret), K(ls_log_stat_info), K(leader), K(to_stop_servers), K(valid_servers));
    (void)snprintf(err_msg, sizeof(err_msg), "Tenant(%lu) LS(%ld) log not sync, %s",
        ls_log_stat_info.get_tenant_id(), ls_log_stat_info.get_ls_id().id(), print_str);
    LOG_USER_ERROR(OB_OP_NOT_ALLOW, err_msg);
  }
  return ret;
}

// valid_servers = member_list - deleted_servers - skip_servers
// (skip_servers include to_stop_servers, servers_in_stopped_zone, stopped_servers, not_alive_servers, not_in_service_servers)
int ObLSStatusOperator::generate_valid_servers_(
    const ObLSReplica::MemberList &member_list,
    const ObZoneManager &zone_mgr,
    const ObServerManager &server_mgr,
    const common::ObIArray<ObAddr> &to_stop_servers,
    common::ObIArray<ObAddr> &valid_servers)
{
  int ret = OB_SUCCESS;
  valid_servers.reset();
  ObArray<ObAddr> invalid_servers;
  if (OB_UNLIKELY(member_list.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("member_list is empty", KR(ret), K(member_list), K(to_stop_servers));
  } else if (OB_FAIL(ObRootUtils::get_invalid_server_list(
      zone_mgr,
      server_mgr,
      invalid_servers))) {
    LOG_WARN("fail to get invalid server list", KR(ret));
  } else {
    ARRAY_FOREACH_N(member_list, idx, cnt) {
      const ObAddr &server = member_list.at(idx).get_server();
      bool is_alive = false;
      if (OB_FAIL(server_mgr.check_server_alive(server, is_alive))) { // filter deleted server which is only in member_list
        LOG_WARN("fail to check is server alive", KR(ret), K(server));
      } else if (!is_alive) {
        LOG_INFO("find not alive server in member_list", K(server), K(member_list));
      } else if (!common::has_exist_in_array(invalid_servers, server)
          && !common::has_exist_in_array(to_stop_servers, server)) {
        if (OB_FAIL(valid_servers.push_back(server))) {
          LOG_WARN("fail to push back", KR(ret), K(server),
              K(invalid_servers), K(to_stop_servers), K(valid_servers));
        }
      }
    }
  }
  LOG_INFO("generate valid servers", KR(ret), K(member_list), K(to_stop_servers), K(valid_servers));
  return ret;
}

int ObLSStatusOperator::construct_ls_leader_info_sql_(common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const char *excluded_status = ls_status_to_str(OB_LS_CREATE_ABORT);
  if (OB_FAIL(sql.assign_fmt(
      "SELECT a.tenant_id, a.ls_id FROM %s AS a LEFT JOIN %s AS b "
      "ON a.tenant_id = b.tenant_id AND a.ls_id = b.ls_id AND b.role = 'LEADER' "
      "WHERE status != '%s' AND role IS NULL "
      "ORDER BY tenant_id, ls_id",
      OB_ALL_VIRTUAL_LS_STATUS_TNAME,
      OB_ALL_VIRTUAL_LOG_STAT_TNAME,
      excluded_status))) {
    LOG_WARN("failed to assign sql", KR(ret), K(sql));
  }
  return ret;
}

int ObLSStatusOperator::check_all_ls_has_leader(
    ObISQLClient &client,
    const char *print_str,
    bool &has_ls_without_leader,
    ObSqlString &error_msg)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  if (OB_FAIL(construct_ls_leader_info_sql_(sql))) {
    LOG_WARN("failed to construct ls paxos info sql", KR(ret), K(sql));
  } else {
    DEBUG_SYNC(BEFORE_CHECK_ALL_LS_HAS_LEADER);
    LOG_INFO("begin to check_all_ls_has_leader",
        K(sql), K(print_str), K(has_ls_without_leader));
    HEAP_VAR(ObMySQLProxy::MySQLResult, res) {
      common::sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(client.read(res, OB_SYS_TENANT_ID, sql.ptr()))) {
        LOG_WARN("failed to read", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get sql result", KR(ret), K(sql));
      } else {
        while (OB_SUCC(ret)) {
          if (OB_FAIL(result->next())) {
            if (OB_UNLIKELY(OB_ITER_END != ret)) {
              LOG_WARN("get next result failed", KR(ret));
            }
          } else {
            uint64_t tenant_id = OB_INVALID_TENANT_ID;
            int64_t ls_id = ObLSID::INVALID_LS_ID;
            EXTRACT_INT_FIELD_MYSQL(*result, "tenant_id", tenant_id, uint64_t);
            EXTRACT_INT_FIELD_MYSQL(*result, "ls_id", ls_id, int64_t);

            if (OB_SUCC(ret)) {
              has_ls_without_leader = true;
              ret = OB_OP_NOT_ALLOW;
              int tmp_ret = OB_SUCCESS;
              LOG_WARN("find ls has no leader when check_all_ls_has_leader",
                  KR(ret), K(tenant_id), K(ls_id));
              if (OB_TMP_FAIL(error_msg.assign_fmt("Tenant(%lu) LS(%ld) has no leader, %s",
                      tenant_id, ls_id, print_str))) {
                LOG_WARN("failed to assign sql", KR(ret), K(tenant_id), K(ls_id), K(print_str));
              }
            }
          }
        } // end while
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    } // end HEAP_VAR
    LOG_INFO("finish to check_all_ls_has_leader",
        KR(ret), K(print_str), K(has_ls_without_leader), K(sql));
  }
  return ret;
}

int ObLSStatusOperator::create_abort_ls_in_switch_tenant(
    const uint64_t tenant_id,
    const share::ObTenantSwitchoverStatus &status,
    const int64_t switchover_epoch,
    ObISQLClient &client)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  if (OB_UNLIKELY(!is_user_tenant(tenant_id) || !status.is_valid() || OB_INVALID_VERSION == switchover_epoch)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(status), K(switchover_epoch));
  } else {
    ObMySQLTransaction trans;
    share::ObLSStatusInfoArray status_info_array;
    ObLSStatusOperator status_op;
    ObAllTenantInfo tenant_info;
    const uint64_t exec_tenant_id = ObLSLifeIAgent::get_exec_tenant_id(tenant_id);
    if (OB_FAIL(trans.start(&client, exec_tenant_id))) {
      LOG_WARN("failed to start trans", KR(ret), K(exec_tenant_id), K(tenant_id));
    } else if (OB_FAIL(ObAllTenantInfoProxy::load_tenant_info(tenant_id, &trans, true, tenant_info))) {
      LOG_WARN("failed to load tenant info", KR(ret), K(tenant_id));
    } else if (OB_UNLIKELY(switchover_epoch != tenant_info.get_switchover_epoch()
                           || status != tenant_info.get_switchover_status())) {
      ret = OB_NEED_RETRY;
      LOG_WARN("switchover may concurrency, need retry", KR(ret), K(switchover_epoch), K(status), K(tenant_info));
    } else if (OB_FAIL(sql.assign_fmt("UPDATE %s set status = '%s',init_member_list = '', b_init_member_list = ''"
                               " where tenant_id = %lu and status in ('%s', '%s')",
                               OB_ALL_LS_STATUS_TNAME,
                               ls_status_to_str(share::OB_LS_CREATE_ABORT),
                               tenant_id, ls_status_to_str(OB_LS_CREATED), ls_status_to_str(OB_LS_CREATING)))) {
      LOG_WARN("failed to assign sql", KR(ret), K(tenant_id), K(sql));
    } else if (OB_FAIL(exec_write(tenant_id, sql, this, trans, true))) {
      LOG_WARN("failed to exec write", KR(ret), K(tenant_id), K(sql));
    }
    if (trans.is_started()) {
      int tmp_ret = OB_SUCCESS;
      if (OB_SUCCESS != (tmp_ret = trans.end(OB_SUCC(ret)))) {
        LOG_WARN("failed to commit trans", KR(ret), KR(tmp_ret));
        ret = OB_SUCC(ret) ? tmp_ret : ret;
      }
    }
  }
  LOG_INFO("finish create abort ls", KR(ret), K(tenant_id), K(sql));

  return ret;
}

int ObLSStatusOperator::check_ls_exist(
    const uint64_t tenant_id,
    const ObLSID &ls_id,
    ObLSExistState &state)
{
  int ret = OB_SUCCESS;
  state.reset();
  schema::ObSchemaGetterGuard schema_guard;
  bool tenant_exist = false;
  ObSqlString sql;
  if (OB_UNLIKELY(!ls_id.is_valid_with_tenant(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid args", KR(ret), K(tenant_id), K(ls_id));
  } else if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("GCTX has null ptr", KR(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_tenant_schema_guard(OB_SYS_TENANT_ID, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", KR(ret));
  } else if (OB_FAIL(schema_guard.check_tenant_exist(tenant_id, tenant_exist))) {
    LOG_WARN("fail to check tenant exist", KR(ret), K(tenant_id));
  } else if (OB_UNLIKELY(!tenant_exist)) {
    ret = OB_TENANT_NOT_EXIST;
    LOG_WARN("tenant not exist", KR(ret), K(tenant_id));
  } else if (OB_FAIL(sql.assign_fmt(
      "SELECT (SELECT COUNT(*) > 0 FROM %s WHERE tenant_id = %lu AND ls_id = %ld) AS ls_is_existing, "
      "MAX(ls_id) < %ld AS ls_is_uncreated FROM %s WHERE tenant_id = %lu",
      OB_ALL_LS_STATUS_TNAME,
      tenant_id,
      ls_id.id(),
      ls_id.id(),
      OB_ALL_LS_STATUS_TNAME,
      tenant_id))) {
    LOG_WARN("assign sql failed", KR(ret), K(tenant_id), K(ls_id), K(sql));
  } else {
    SMART_VAR(ObISQLClient::ReadResult, result) {
      bool ls_is_existing = false;
      bool ls_is_uncreated = false;
      common::sqlclient::ObMySQLResult *res = NULL;
      uint64_t exec_tenant_id = ObLSLifeIAgent::get_exec_tenant_id(tenant_id);
      if (OB_FAIL(GCTX.sql_proxy_->read(result, exec_tenant_id, sql.ptr()))) {
        LOG_WARN("execute sql failed", KR(ret),
            K(tenant_id), K(ls_id), K(exec_tenant_id), K(sql));
      } else if (OB_ISNULL(res = result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get mysql result failed", KR(ret),
            K(tenant_id), K(ls_id), K(exec_tenant_id), K(sql));
      } else if (OB_FAIL(res->next())) {
        LOG_WARN("next failed", KR(ret), K(tenant_id), K(ls_id), K(sql));
      } else if (OB_FAIL(res->get_bool("ls_is_existing", ls_is_existing))) {
        LOG_WARN("fail to get ls_is_existing", KR(ret), K(tenant_id), K(ls_id));
      } else if (OB_FAIL(res->get_bool("ls_is_uncreated", ls_is_uncreated))) {
        LOG_WARN("fail to get ls_is_uncreated", KR(ret), K(tenant_id), K(ls_id));
      } else if (ls_is_existing) {
        state.set_existing();
      } else if (ls_is_uncreated) {
        state.set_uncreated();
      } else {
        state.set_deleted();
      }
      LOG_INFO("check ls exist finished", KR(ret),
          K(tenant_id), K(ls_id), K(state), K(ls_is_existing), K(ls_is_uncreated));
    }
  }
  return ret;
}

}//end of share
}//end of ob
