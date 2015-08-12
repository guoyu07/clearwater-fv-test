/**
 * Project Clearwater - IMS in the cloud.
 * Copyright (C) 2015  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "snmp_event_accumulator_table.h"
#include "snmp_continuous_accumulator_table.h"
#include "snmp_counter_table.h"
#include "snmp_success_fail_count_table.h"
#include "snmp_ip_count_table.h"
#include "snmp_scalar.h"
#include "test_interposer.hpp"
#include "snmp_single_count_by_node_type_table.h"
#include "snmp_success_fail_count_by_request_type_table.h"

#ifdef READ
#error "netsnmp includes have polluted the namespace!"
#endif

#include "snmp_internal/snmp_includes.h"
using ::testing::AnyOf;
using ::testing::Contains;

static void* snmp_thread(void* data)
{
  while (1)
  {
    agent_check_and_process(1);
  }
  return NULL;
}


static pthread_t thr;
std::string test_oid = ".1.2.2";

class SNMPTest : public ::testing::Test
{
  SNMPTest() {};

  // Sets up an SNMP master agent on port 16161 for us to register tables with and query
  static void SetUpTestCase()
  {
    // Configure SNMPd to use the fvtest.conf in the local directory
    char cwd[256];
    getcwd(cwd, sizeof(cwd));
    netsnmp_ds_set_string(NETSNMP_DS_LIBRARY_ID,
                          NETSNMP_DS_LIB_CONFIGURATION_DIR,
                          cwd);

    // Log SNMPd output to a file
    snmp_enable_filelog("fvtest-snmpd.out", 0);

    init_agent("fvtest");
    init_snmp("fvtest");
    init_master_agent();

    // Run a thread to handle SNMP requests
    pthread_create(&thr, NULL, snmp_thread, NULL);
  }

  static void TearDownTestCase()
  {
    pthread_cancel(thr);
    pthread_join(thr, NULL);
    snmp_shutdown("fvtest");
  }

};

TEST_F(SNMPTest, ScalarValue)
{
  // Create a scalar
  SNMP::U32Scalar scalar("answer", test_oid);
  scalar.value = 42;

  // Shell out to snmpget to query that scalar
  FILE* fd = popen("snmpget -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  char buf[1024];
  fgets(buf, sizeof(buf), fd);

  // Check that it has the right OID, value and type
  ASSERT_STREQ(".1.2.2 = Gauge32: 42\n", buf);
}


TEST_F(SNMPTest, TableOrdering)
{
  // Create a table
  SNMP::EventAccumulatorTable* tbl = SNMP::EventAccumulatorTable::create("latency", test_oid);

  // Shell out to snmpwalk to find all entries in that table
  FILE* fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  char buf[1024];

  // Check that they come in the right order - column 2 of row 1, column 2 of row 2, column 2 of row
  // 3, column 3 of row 1, column 3 of row 2....
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.3 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2 = Gauge32: 0\n", buf);

  delete tbl;
}

TEST_F(SNMPTest, LatencyCalculations)
{
  cwtest_completely_control_time();

  char buf[1024];
  FILE* fd;

  // Create a table
  SNMP::EventAccumulatorTable* tbl = SNMP::EventAccumulatorTable::create("latency", test_oid);

  // Just put one sample in (which should have a variance of 0).
  tbl->accumulate(100);

  // Move on five seconds. The "previous five seconds" stat should now reflect that sample.
  cwtest_advance_time_ms(5000);

  // Average should be 100
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 100\n", buf);
  // Variance should be 0
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1 = Gauge32: 0\n", buf);

  // Now input two samples in this latency period.
  tbl->accumulate(300);
  tbl->accumulate(500);

  // Move on five seconds. The "previous five seconds" stat should now reflect those two samples.
  cwtest_advance_time_ms(5000);

  // Average should be 400
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 400\n", buf);
  // HWM should be 500
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1 = Gauge32: 500\n", buf);
  // LWM should be 300
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.1 = Gauge32: 300\n", buf);
  // Count should be 2
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.6.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.6.1 = Gauge32: 2\n", buf);

  cwtest_reset_time();
  delete tbl;
}

TEST_F(SNMPTest, CounterTimePeriods)
{
  cwtest_completely_control_time();
  // Create a table indexed by time period
  SNMP::CounterTable* tbl = SNMP::CounterTable::create("counter", test_oid);

  // Shell out to snmpwalk to find all entries in that table
  FILE* fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  char buf[1024];

  // At first, all three rows (previous 5s, current 5m, previous 5m) should have a zero value
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.3 = Gauge32: 0\n", buf);

  // Increment the counter. This should show up in the current-five-minute stats, but nowhere else.
  tbl->increment();

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 1\n", buf); // Current 5 minutes
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.3 = Gauge32: 0\n", buf);

  // Move on five seconds. The "previous five seconds" stat should now also reflect the increment.
  cwtest_advance_time_ms(5000);

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 1\n", buf); // Current 5 minutes
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.3 = Gauge32: 0\n", buf);

  // Move on five more seconds. The "previous five seconds" stat should no longer reflect the increment.
  cwtest_advance_time_ms(5000);

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 1\n", buf); // Current 5 minutes
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.3 = Gauge32: 0\n", buf);

  // Move on five minutes. Only the "previous five minutes" stat should now reflect the increment.
  cwtest_advance_time_ms(300000);

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.3 = Gauge32: 1\n", buf);

  // Increment the counter again and move on ten seconds
  tbl->increment();
  cwtest_advance_time_ms(10000);

  // That increment shouldn't be in the "previous 5 seconds" stat (because it was made 10 seconds
  // ago).
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 0\n", buf);

  cwtest_reset_time();
  delete tbl;
}

TEST_F(SNMPTest, IPCountTable)
{
  // Create a table
  SNMP::IPCountTable* tbl = SNMP::IPCountTable::create("ip-counter", test_oid);

  tbl->get("127.0.0.1")->increment();

  // Shell out to snmpwalk to find all entries in that table
  FILE* fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  char buf[1024];

  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.4.127.0.0.1 = Gauge32: 1\n", buf);
  delete tbl;
}

TEST_F(SNMPTest, SuccessFailCountTable)
{
  // Create table
  SNMP::SuccessFailCountTable* tbl = SNMP::SuccessFailCountTable::create("success_fail_count", test_oid);

  // Shell out to snmpwalk to find all entries in that table
  FILE* fd;
  char buf[1024];

  tbl->increment_attempts();
  tbl->increment_successes();
  tbl->increment_attempts();
  tbl->increment_failures();
  // Should be 2 attempt, 1 success, 1 failures.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 2\n", buf);
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2 = Gauge32: 1\n", buf);
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.2 = Gauge32: 1\n", buf);

  // Move on five seconds. The "previous five seconds" stat should now also reflect the increment.
  cwtest_advance_time_ms(5000);

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.1 = Gauge32: 2\n", buf);
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1 = Gauge32: 1\n", buf);
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1 = Gauge32: 1\n", buf);

  delete tbl;
}

TEST_F(SNMPTest, SingleCountByNodeTypeTable)
{
  cwtest_completely_control_time();

  // Create a table
  SNMP::SingleCountByNodeTypeTable* tbl = SNMP::SingleCountByNodeTypeTable::create("single-count", test_oid, {SNMP::NodeTypes::SCSCF, SNMP::NodeTypes::ICSCF});

  // Shell out to snmpwalk to find all entries in that table
  FILE* fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");
  char buf[1024];

  // To start with, all values should be 0.
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.2 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.2 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.2 = Gauge32: 0\n", buf);

  // Add an entry for each supported node type. Only the current five minutes
  // should have a count.
  tbl->increment(SNMP::NodeTypes::SCSCF);
  tbl->increment(SNMP::NodeTypes::ICSCF);

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");

  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.2 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.0 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.2 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.2 = Gauge32: 0\n", buf);

  // Move on five seconds. The "previous five seconds" stat should now also reflect the increment.
  cwtest_advance_time_ms(5000);
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2", "r");

  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.0 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.2 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.0 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.2 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.2 = Gauge32: 0\n", buf);

  cwtest_reset_time();
  delete tbl;
}

TEST_F(SNMPTest, SuccessFailCountByRequestTypeTable)
{
  cwtest_completely_control_time();

  // Create a table
  SNMP::SuccessFailCountByRequestTypeTable* tbl = SNMP::SuccessFailCountByRequestTypeTable::create("success_fail_by_request", test_oid);
  FILE* fd;
  char buf[1024];

  // To start with, all values should be 0 (check the INVITE and ACK entries).
  // Shell out to snmpwalk to find previous 5 second period attempts.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 second period successes.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 second period failures.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.1.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find current 5 minute period attempts.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find current 5 minute period successes.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.2.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.2.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find current 5 minute period failures.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.2.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.2.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 minute period attempts.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.3", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.3.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 minute period successes.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.3", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.3.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.3.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 minute period failures.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.3", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.3.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.3.1 = Gauge32: 0\n", buf);

  // Increment an attempt and success for INVITE, and an attempt and failure for ACK. Only the current five minutes should have a count.
  tbl->increment_attempts(SNMP::SIPRequestTypes::INVITE);
  tbl->increment_successes(SNMP::SIPRequestTypes::INVITE);
  tbl->increment_attempts(SNMP::SIPRequestTypes::ACK);
  tbl->increment_failures(SNMP::SIPRequestTypes::ACK);

  // Shell out to snmpwalk to find previous 5 second period attempts.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 second period successes.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 second period failures.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.1.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find current 5 minute period attempts.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.0 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2.1 = Gauge32: 1\n", buf);

  // Shell out to snmpwalk to find current 5 minute period successes.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.2.0 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.2.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find current 5 minute period failures.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.2.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.2.1 = Gauge32: 1\n", buf);

  // Move on five seconds. The "previous five seconds" stat should now also reflect the increment.
  cwtest_advance_time_ms(5000);

  // Shell out to snmpwalk to find previous 5 second period attempts.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.0 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.1.1 = Gauge32: 1\n", buf);

  // Shell out to snmpwalk to find previous 5 second period successes.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1.0 = Gauge32: 1\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.1.1 = Gauge32: 0\n", buf);

  // Shell out to snmpwalk to find previous 5 second period failures.
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.1", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.1.0 = Gauge32: 0\n", buf);
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.1.1 = Gauge32: 1\n", buf);

  cwtest_reset_time();
  delete tbl;
}

// Advance to the next start of interval - accurate to within the first
// second. i.e. May jump to 12:00:00:634, but never before 12:00:00:000
void jump_to_next_periodstart(uint32_t interval_ms)
{
  struct timespec now;
  clock_gettime(CLOCK_REALTIME_COARSE, &now);

  // Calculate the current time in ms
  uint64_t ms_since_epoch = (now.tv_sec * 1000) + (now.tv_nsec / 1000000);

  // Move time forward
  cwtest_advance_time_ms(interval_ms - (ms_since_epoch % interval_ms));
}

TEST_F(SNMPTest, ContinuousAccumulatorTable)
{
  cwtest_completely_control_time();

  // Consider a 5 minute period
  jump_to_next_periodstart(300000);

  // Advance to 59s into the period
  cwtest_advance_time_ms(59000);

  // Create table at this point
  SNMP::ContinuousAccumulatorTable* tbl = SNMP::ContinuousAccumulatorTable::create("continuous", test_oid);

  // Add one value to the table and advance 120 seconds
  tbl->accumulate(100);
  cwtest_advance_time_ms(120000);

  // Average should not take into account the time before the table was created
  // (therefore average should still be 100)
  FILE* fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.2", "r");
  char buf[1024];
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 100\n", buf); // Current 5 minutes

  // Add another value to the table and advance 120 seconds
  tbl->accumulate(200);
  cwtest_advance_time_ms(120000);

  // This period has spent half the time at 200, and half at 100
  // So average value should be 150
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 150\n", buf); // Current 5 minutes

  // Jump forward to the next period, and move halfway through it
  // As there is only a second left of this period, the average should not
  // change
  jump_to_next_periodstart(300000);

  cwtest_advance_time_ms(150000);

  // The average value should be 200 for the current 5 minutes as it is carried
  // over, additionally, should be value for HWM and LWM, and variance 0
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 200\n", buf); // Current 5 minutes - Avg

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.3.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.3.2 = Gauge32: 0\n", buf); // Current 5 minutes - Variance

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.2 = Gauge32: 200\n", buf); // Current 5 minutes - HWM

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.2 = Gauge32: 200\n", buf); // Current 5 minutes - LWM

  // Add a HWM and LWM 5 seconds apart
  tbl->accumulate(150);
  cwtest_advance_time_ms(5000);

  tbl->accumulate(250);
  cwtest_advance_time_ms(5000);

  // The average value should remain at 200, but the LWM and HWM are adjusted
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.2 = Gauge32: 200\n", buf); // Current 5 minutes - Avg

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.4.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.4.2 = Gauge32: 250\n", buf); // Current 5 minutes - HWM

  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.5.2", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.5.2 = Gauge32: 150\n", buf); // Current 5 minutes - LWM

  // The previous 5 minutes should not have changed value
  fd = popen("snmpwalk -v2c -On -c clearwater 127.0.0.1:16161 .1.2.2.1.2.3", "r");
  fgets(buf, sizeof(buf), fd);
  ASSERT_STREQ(".1.2.2.1.2.3 = Gauge32: 150\n", buf); // Previous 5 minutes - Avg

  cwtest_reset_time();
  delete tbl;

}
