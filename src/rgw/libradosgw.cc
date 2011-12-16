#include "include/atomic.h"

#include "libradosgw.hpp"

#include "rgw_rados.h"
#include "rgw_cache.h"

#define USER_INFO_POOL_NAME ".users"
#define USER_INFO_EMAIL_POOL_NAME ".users.email"
#define USER_INFO_SWIFT_POOL_NAME ".users.swift"
#define USER_INFO_UID_POOL_NAME ".users.uid"
#define RGW_USER_ANON_ID "anonymous"


namespace libradosgw {

  static rgw_bucket ui_key_bucket(USER_INFO_POOL_NAME);
  static rgw_bucket ui_email_bucket(USER_INFO_EMAIL_POOL_NAME);
  static rgw_bucket ui_swift_bucket(USER_INFO_SWIFT_POOL_NAME);
  static rgw_bucket ui_uid_bucket(USER_INFO_UID_POOL_NAME);

  rgw_bucket rgw_root_bucket(RGW_ROOT_BUCKET);

  void encode(const AccessKey& k, bufferlist& bl) {
     __u32 ver = 1;
    ::encode(ver, bl);
    ::encode(k.id, bl);
    ::encode(k.key, bl);
    ::encode(k.subuser, bl);
  }

  void decode(AccessKey& k, bufferlist::iterator& bl) {
     __u32 ver;
     ::decode(ver, bl);
     ::decode(k.id, bl);
     ::decode(k.key, bl);
     ::decode(k.subuser, bl);
  }
  
  void encode(const SubUser& s, bufferlist& bl) {
     __u32 ver = 1;
    ::encode(ver, bl);
    ::encode(s.name, bl);
    ::encode(s.perm_mask, bl);
  }

  void decode(SubUser& s, bufferlist::iterator& bl) {
     __u32 ver;
     ::decode(ver, bl);
     ::decode(s.name, bl);
     ::decode(s.perm_mask, bl);
  }

  struct BucketImpl {
  };

  struct AccountImpl : public RefCountedObject
  {
    StoreImpl *store;

    AccountImpl(StoreImpl *s);
    ~AccountImpl();


    int store_info(Account *account);

    void encode(Account *account, bufferlist& bl) const {
      __u32 ver = USER_INFO_VER;

      User& user = account->user;

      ::encode(ver, bl);
      ::encode(user.auid, bl);
      string access_key;
      string secret_key;
      if (!account->access_keys.empty()) {
        map<string, AccessKey>::const_iterator iter = account->access_keys.begin();
        const AccessKey& k = iter->second;
        access_key = k.id;
        secret_key = k.key;
      }
      ::encode(access_key, bl);
      ::encode(secret_key, bl);
      ::encode(user.display_name, bl);
      ::encode(user.email, bl);
      string swift_name;
      string swift_key;
      if (!account->swift_keys.empty()) {
        map<string, AccessKey>::const_iterator iter = account->swift_keys.begin();
        const AccessKey& k = iter->second;
        swift_name = k.id;
        swift_key = k.key;
      }
      ::encode(swift_name, bl);
      ::encode(swift_key, bl);
      ::encode(user.uid, bl);
      ::encode(account->access_keys, bl);
      ::encode(account->subusers, bl);
      ::encode(account->suspended, bl);
      ::encode(account->swift_keys, bl);
    }
    void decode(Account *account, bufferlist::iterator& bl) {
       __u32 ver;
      ::decode(ver, bl);

      User& user = account->user;

      if (ver >= 2) ::decode(account->auid, bl);
        else account->auid = CEPH_AUTH_UID_DEFAULT;
      string access_key;
      string secret_key;
      ::decode(access_key, bl);
      ::decode(secret_key, bl);
      if (ver < 6) {
        AccessKey k;
        k.id = access_key;
        k.key = secret_key;
        account->access_keys[access_key] = k;
      }
      ::decode(user.display_name, bl);
      ::decode(user.email, bl);
      string swift_name;
      string swift_key;
      if (ver >= 3) ::decode(swift_name, bl);
      if (ver >= 4) ::decode(swift_key, bl);
      if (ver >= 5)
        ::decode(user.uid, bl);
      else
        user.uid = access_key;
      if (ver >= 6) {
        ::decode(account->access_keys, bl);
        ::decode(account->subusers, bl);
      }
      account->suspended = false;
      if (ver >= 7) {
        ::decode(account->suspended, bl);
      }
      if (ver >= 8) {
        ::decode(account->swift_keys, bl);
      }
    }
  };

  class StoreImpl : public RefCountedObject {
    RGWRados *access;

    int account_from_index(string& key, rgw_bucket& bucket, Account& account);

  public:

    StoreImpl() : access(NULL) {}

    int init(CephContext *cct) {
      int use_cache = cct->_conf->rgw_cache_enabled;

      if (use_cache) {
        access = new RGWRados;
      } else {
	access = new RGWCache<RGWRados>;
      }

      int ret = access->initialize(cct);

      return ret;
    }

    void shutdown() {
      if (!access)
	return;

      access->finalize();
      access = NULL;
    }

    int put_complete_obj(string& uid, rgw_bucket& bucket, string& oid, const char *data, size_t size);
    int get_complete_obj(void *ctx, rgw_bucket& bucket, string& key, bufferlist& bl);

    int account_by_uid(string& name, Account& account) {
      return account_from_index(name, ui_uid_bucket, account);
    }
    int account_by_email(string& email, Account& account) {
      return account_from_index(email, ui_email_bucket, account);
    }
    int account_by_access_key(string& access_key, Account& account) {
      return account_from_index(access_key, ui_key_bucket, account);
    }
    int account_by_subuser(string& subuser, Account& account) {
      return account_from_index(subuser, ui_swift_bucket, account);
    }

    int user_by_uid(string& name, User& user) {
      Account account;
      int r = account_by_uid(name, account);
      if (r < 0)
	return r;
      user = account.user;
      return 0;
    }
    int user_by_email(string& email, User& user) {
      Account account;
      int r = account_by_email(email, account);
      if (r < 0)
	return r;
      user = account.user;
      return 0;
    }
    int user_by_access_key(string& access_key, User& user) {
      Account account;
      int r = account_by_access_key(access_key, account);
      if (r < 0)
	return r;
      user = account.user;
      return 0;
    }
    int user_by_subuser(string& subuser, User& user) {
       Account account;
      int r = account_by_subuser(subuser, account);
      if (r < 0)
	return r;
      user = account.user;
      return 0;
    }
  };

  int StoreImpl::put_complete_obj(string& uid, rgw_bucket& bucket, string& oid, const char *data, size_t size)
  {
    map<string,bufferlist> attrs;

    rgw_obj obj(bucket, oid);

    int ret = access->put_obj(NULL, obj, data, size, NULL, attrs);

    if (ret == -ENOENT) {
      ret = access->create_bucket(uid, bucket, attrs, true); //all callers are using system buckets
      if (ret >= 0)
        ret = access->put_obj(NULL, obj, data, size, NULL, attrs);
    }

    return ret;
  }

  int StoreImpl::get_complete_obj(void *ctx, rgw_bucket& bucket, string& key, bufferlist& bl)
  {
    int ret;
    char *data = NULL;
    struct rgw_err err;
    void *handle = NULL;
    bufferlist::iterator iter;
#define READ_CHUNK_LEN (16 * 1024)
    int request_len = READ_CHUNK_LEN;
    rgw_obj obj(bucket, key);
    ret = access->prepare_get_obj(ctx, obj, NULL, NULL, NULL, NULL,
                                  NULL, NULL, NULL, NULL, NULL, NULL, &handle, &err);
    if (ret < 0)
      return ret;

    do {
      ret = access->get_obj(ctx, &handle, obj, &data, 0, request_len - 1);
      if (ret < 0)
        goto done;
      if (ret < request_len)
        break;
      free(data);
      request_len *= 2;
    } while (true);

    bl.append(data, ret);
    free(data);

    ret = 0;
  done:
    access->finish_get_obj(&handle);
    return ret;
  }

  int StoreImpl::account_from_index(string& key, rgw_bucket& bucket, Account& account)
  {
    bufferlist bl;
    string uid;

    int ret = get_complete_obj(NULL, bucket, key, bl);
    if (ret < 0)
      return ret;

    AccountImpl *impl = NULL;

    bufferlist::iterator iter = bl.begin();
    try {
      ::decode(uid, iter);
      if (!iter.end()) {
	impl = new AccountImpl(this);
        impl->decode(&account, iter);
	account.impl = impl;
      }
    } catch (buffer::error& err) {
      delete impl;
      dout(0) << "ERROR: failed to decode account info, caught buffer::error" << dendl;
      return -EIO;
    }

    return 0;
  }

  int Store::init(CephContext *cct) {
    impl = new StoreImpl;
    return impl->init(cct);
  }

  void Store::shutdown() {
    impl->shutdown();
    impl->put();
  }

  int Store::account_by_uid(string& name, Account& account) {
    return impl->account_by_uid(name, account);
  }

  int Store::account_by_email(string& email, Account& account) {
    return impl->account_by_email(email, account);
  }

  int Store::account_by_access_key(string& access_key, Account& account) {
    return impl->account_by_access_key(access_key, account);
  }

  int Store::account_by_subuser(string& access_key, Account& account) {
    return impl->account_by_subuser(access_key, account);
  }

  int Store::user_by_uid(string& name, User& user) {
    return impl->user_by_uid(name, user);
  }

  int Store::user_by_email(string& email, User& user) {
    return impl->user_by_email(email, user);
  }

  int Store::user_by_access_key(string& access_key, User& user) {
    return impl->user_by_access_key(access_key, user);
  }

  int Store::user_by_subuser(string& access_key, User& user) {
    return impl->user_by_subuser(access_key, user);
  }

  AccountImpl::AccountImpl(StoreImpl *s) : store(s) {
    if (store)
      store->get();
  }

  AccountImpl::~AccountImpl() {
      if (store)
	store->put();
  }

  int AccountImpl::store_info(Account *account)
  {
    bufferlist bl;
    encode(account, bl);
    string md5;
    int ret;
    map<string,bufferlist> attrs;

    User& user = account->user;

    map<string, AccessKey>::iterator iter;
    for (iter = account->swift_keys.begin(); iter != account->swift_keys.end(); ++iter) {
      AccessKey& k = iter->second;
      /* check if swift mapping exists */
      User u;
      int r = store->user_by_subuser(k.id, u);
      if (r >= 0 && u.uid.compare(user.uid) != 0) {
        dout(0) << "can't store user info, subuser id already mapped to another user" << dendl;
        return -EEXIST;
      }
    }

    if (account->access_keys.size()) {
      /* check if access keys already exist */
      User u;
      map<string, AccessKey>::iterator iter = account->access_keys.begin();
      for (; iter != account->access_keys.end(); ++iter) {
        AccessKey& k = iter->second;
        int r = store->user_by_access_key(k.id, u);
        if (r >= 0 && u.uid.compare(user.uid) != 0) {
          dout(0) << "can't store user info, access key already mapped to another user" << dendl;
          return -EEXIST;
        }
      }
    }

    bufferlist uid_bl;
    ::encode(user.uid, uid_bl);
    encode(account, uid_bl);

    ret = store->put_complete_obj(user.uid, ui_uid_bucket, user.uid, uid_bl.c_str(), uid_bl.length());
    if (ret < 0)
      return ret;

    if (user.email.size()) {
      ret = store->put_complete_obj(user.uid, ui_email_bucket, user.email, uid_bl.c_str(), uid_bl.length());
      if (ret < 0)
        return ret;
    }

    if (account->access_keys.size()) {
      map<string, AccessKey>::iterator iter = account->access_keys.begin();
      for (; iter != account->access_keys.end(); ++iter) {
        AccessKey& k = iter->second;
        ret = store->put_complete_obj(k.id, ui_key_bucket, k.id, uid_bl.c_str(), uid_bl.length());
        if (ret < 0)
          return ret;
      }
    }

    map<string, AccessKey>::iterator siter;
    for (siter = account->swift_keys.begin(); siter != account->swift_keys.end(); ++siter) {
      AccessKey& k = siter->second;
      ret = store->put_complete_obj(user.uid, ui_swift_bucket, k.id, uid_bl.c_str(), uid_bl.length());
      if (ret < 0)
        return ret;
    }

    return ret;
  }

  User User::Anonymous(true);

  int Account::store_info() {
    return impl->store_info(this);
  }

  int Account::fetch_buckets(RGWUserBuckets& buckets, bool need_stats)
  {
    int ret;
    buckets.clear();
    string buckets_obj_id;
    get_buckets_obj(user.uid, buckets_obj_id);
    bufferlist bl;
#define LARGE_ENOUGH_LEN (4096 * 1024)
    size_t len = LARGE_ENOUGH_LEN;
    rgw_obj obj(ui_uid_bucket, buckets_obj_id);

    do {
      ret = rgwstore->read(NULL, obj, 0, len, bl);
      if (ret < 0)
        return ret;

      if ((size_t)ret != len)
        break;

      len *= 2;
    } while (1);

    bufferlist::iterator p = bl.begin();
    bufferlist header;
    map<string,bufferlist> m;
    try {
      ::decode(header, p);
      ::decode(m, p);
      for (map<string,bufferlist>::iterator q = m.begin(); q != m.end(); q++) {
        bufferlist::iterator iter = q->second.begin();
        RGWBucketEnt bucket;
        ::decode(bucket, iter);
        buckets.add(bucket);
      }
    } catch (buffer::error& err) {
      dout(0) << "ERROR: failed to decode bucket information, caught buffer::error" << dendl;
      return -EIO;
    }

  done:
    list<string> buckets_list;

    if (need_stats) {
     map<string, RGWBucketEnt>& m = buckets.get_buckets();
     int r = rgwstore->update_containers_stats(m);
     if (r < 0)
       dout(0) << "could not get stats for buckets" << dendl;

    }
    return 0;
  }

  class AccoutIteratorImpl : public RefCountedObject {
    AccountImpl *account;
    map<string, BucketInfo> buckets;
    map<string, BucketInfo>::iterator iter;
  public:
    AccountIteratorImpl(AccountImpl *a) {
      a = account;
      if (account)
	account->get();
    }
    ~AccountIteratorImpl() {
      if (account)
	account->put();
    }
    int init() {

    }
    bool next(BucketInfo& bucket) {
    }
  };

  AccountIterator& AccountIterator::operator++() {
   (*impl)++;
   return *this;
  }
  const BucketInfo& operator*() const {

  }

  AccountIterator Account::buckets_begin() {
    AccountIterator iter(impl);

  }
}