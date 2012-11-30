#ifndef CEPH_TEST_LIBCEPHFS_TEST_H
#define CEPH_TEST_LIBCEPHFS_TEST_H
#include <sys/types.h>
#include <unistd.h>
#include <sstream>
#include <string>
#include <algorithm>
#include "tools/common.h"
#include "json_spirit/json_spirit.h"

/*
 * The bool parameter to control localized reads isn't used in
 * ConfiguredMountTest, and is only really needed in MountedTest. This is
 * possible with gtest 1.6, but not 1.5 Maybe time to upgrade.
 */
class ConfiguredMountTest : public ::testing::TestWithParam<bool> {
  protected:
    struct ceph_mount_info *cmount;

    virtual void SetUp() {
      ASSERT_EQ(ceph_create(&cmount, NULL), 0);
      ASSERT_EQ(ceph_conf_read_file(cmount, NULL), 0);
    }

    virtual void TearDown() {
      ASSERT_EQ(ceph_release(cmount), 0);
    }

    void RefreshMount() {
      ASSERT_EQ(ceph_release(cmount), 0);
      ASSERT_EQ(ceph_create(&cmount, NULL), 0);
      ASSERT_EQ(ceph_conf_read_file(cmount, NULL), 0);
    }
};

class MountedTest : public ConfiguredMountTest {

  std::string root, asok;

  protected:
    virtual void SetUp() {
      /* Grab test names to build clean room directory name */
      const ::testing::TestInfo* const test_info =
        ::testing::UnitTest::GetInstance()->current_test_info();

      /* Create unique string using test/testname/pid */
      std::stringstream ss_unique;
      ss_unique << test_info->test_case_name() << "_" << test_info->name() << "_" << getpid();
      std::string unique_path = ss_unique.str();
      std::replace(unique_path.begin(), unique_path.end(), '/', '_');

      /* Make absolute directory for mount root point */
      root = unique_path;
      root.insert(0, 1, '/');

      /* Make /tmp path for client admin socket */
      asok = unique_path;
      asok.insert(0, "/tmp/");

      /* Now mount */
      ConfiguredMountTest::SetUp();
      Mount();
    }

    virtual void TearDown() {
      ASSERT_EQ(ceph_unmount(cmount), 0);
      ConfiguredMountTest::TearDown();
    }

    void Remount(bool deep=false) {
      ASSERT_EQ(ceph_unmount(cmount), 0);
      if (deep)
        ConfiguredMountTest::RefreshMount();
      Mount();
    }

    uint64_t get_objecter_replica_ops() {
      /* Grab and parse the perf data if we haven't already */
      std::stringstream ss;
      ceph_tool_do_admin_socket(asok, "perf dump", ss);
      std::string perfdump = ss.str();

      json_spirit::mValue perfval;
      json_spirit::read(perfdump, perfval);

      json_spirit::mValue objecterVal = perfval.get_obj().find("objecter")->second;
      json_spirit::mValue replicaVal = objecterVal.get_obj().find("op_send_replica")->second;

      return replicaVal.get_uint64();
    }

  private:
    void Mount() {
      /* Setup clean room root directory */
      ASSERT_EQ(ceph_mount(cmount, "/"), 0);

      struct stat st;
      int ret = ceph_stat(cmount, root.c_str(), &st);
      if (ret == -ENOENT)
        ASSERT_EQ(ceph_mkdir(cmount, root.c_str(), 0700), 0);
      else {
        ASSERT_EQ(ret, 0);
        ASSERT_TRUE(S_ISDIR(st.st_mode));
      }

      /* Create completely fresh mount context */
      ASSERT_EQ(ceph_unmount(cmount), 0);
      ConfiguredMountTest::RefreshMount();

      /* Setup admin socket */
      ASSERT_EQ(ceph_conf_set(cmount, "admin_socket", asok.c_str()), 0);

      /* Mount with new root directory */
      ASSERT_EQ(ceph_mount(cmount, root.c_str()), 0);

      /* Use localized reads for this mount? */
      bool localize = GetParam();
      ASSERT_EQ(ceph_localize_reads(cmount, localize), 0);
    }
};

#endif