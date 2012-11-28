#ifndef CEPH_TEST_LIBCEPHFS_TEST_H
#define CEPH_TEST_LIBCEPHFS_TEST_H

class ConfiguredMountTest : public ::testing::Test {
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
  protected:
    virtual void SetUp() {
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

  private:
    void Mount() {
      ASSERT_EQ(ceph_mount(cmount, NULL), 0);
    }
};

#endif
