#include "admin_user.h"
#include "digestpp.hpp"
#include "tools/sf_logger.hpp"


admin_user_manager::admin_user_manager(const string &path) {
    sf_info("database path", path);
    storage__ = make_unique<StorageType >(init_user_storage(path));
    storage__->sync_schema();

    if(check_empty())
    {
        admin_user user{
            0,
            "skyfire",
            hash_password("123456"),
            "1513008876",
            "skyfireitdiy.cn",
            ""
        };
        storage__->insert(user);
    }
}

bool admin_user_manager::check_empty() {
    return storage__->count<admin_user>() == 0;
}

shared_ptr<admin_user> admin_user_manager::check_user(const string &name, const string &password) {
    auto users =  storage__->get_all<admin_user>(where(c(&admin_user::name) == name and c(&admin_user::password) == hash_password(password)));
    if(users.empty())
    {
        return nullptr;
    }
    return std::make_shared<admin_user>(users[0]);
}

string hash_password(const std::string &password) {
    return digestpp::sha512().absorb(password).hexdigest();
}