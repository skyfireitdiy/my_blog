
#include "blog_server.h"
#include "blog_config.h"
#include "database.h"
#include "network/sf_http_part_router.hpp"
#include "network/sf_websocket_router.hpp"
#include "tools/sf_finally.hpp"
#include "tools/sf_logger.hpp"

using namespace skyfire;
using namespace std;

blog_server::blog_server(const std::string &config_file_path) {
    env.set_expression("<<", ">>");

    string TZ_ENV{"TZ=Asia/Shanghai"};
    putenv(TZ_ENV.data());

    config__ = config_manager::make_instance(config_file_path);
    if (!config__->inited()) {
        sf_error("config file load error", config_file_path);
        assert(false);
    }

    auto server_json_config = config__->value("server"s);

    if (server_json_config.is_null()) {
        sf_error("server config error");
        assert(false);
    }

    auto blog_json_config = config__->value("blog"s);
    if (blog_json_config.is_null()) {
        sf_error("blog config error");
        assert(false);
    }

    from_json(blog_json_config, blog_config__);

    database__ = database::get_instance(
        sf_path_join(blog_config__.db_path, "database.db3"s));
    if (database__->check_user_empty()) {
        auto default_user_json = config__->value("default_user"s);
        if (default_user_json.is_null()) {
            sf_error("default_user config error");
            assert(false);
        }
        sf_info("user empty, create default user:", default_user_json);
        admin_user user;
        from_json(default_user_json, user);
        user.id = -1;
        user.password = hash_password(user.password);
        database__->insert_user(user);
    }

    if (database__->check_blog_info_empty()) {
        auto blog_info_json = config__->value("blog_info"s);
        if (blog_info_json.is_null()) {
            sf_error("blog info config error");
            assert(false);
        }
        sf_info("blog_info empty, create default user:", blog_info_json);
        blog_info info;
        from_json(blog_info_json, info);
        database__->insert_blog_info(info);
    }

    sf_http_server_config server_conf;
    from_json(server_json_config, server_conf);

    if (server_conf.port == 0) {
        server_conf.port = 80;
    }
    if (server_conf.host.empty()) {
        server_conf.host = "0.0.0.0";
    }
    if (server_conf.session_timeout == 0) {
        server_conf.session_timeout = 3600;
    }
    if (server_conf.tmp_file_path.empty()) {
        server_conf.tmp_file_path = "/tmp";
    }

    setup_server(server_conf);
}

void blog_server::setup_server(const sf_http_server_config &server_conf) {
    server__ = sf_http_server::make_instance(server_conf);

    auto root_router = sf_http_part_router::make_instance(
        "/"s, [this](const sf_http_request &req, sf_http_response &res) {
            return true;
        });
    root_router->add_router(
        sf_static_router::make_instance(blog_config__.static_path));
    root_router->add_router(sf_http_router::make_instance(
        "/"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            res.redirect("/html/index.html");
        })));

    auto admin_router = sf_http_part_router::make_instance(
        "/admin"s, [this](const sf_http_request &req, sf_http_response &res) {
            return admin_check(req, res);
        });
    admin_router->add_router(
        sf_static_router::make_instance(blog_config__.static_path));

    admin_router->add_router(sf_http_router::make_instance(
        "/"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            admin_root(req, res);
        })));

    root_router->add_router(admin_router);

    auto api_router = sf_http_part_router::make_instance(
        "/api"s, [this](const sf_http_request &req, sf_http_response &res) {
            return true;
        });
    api_router->add_router(sf_http_router::make_instance(
        "/login"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            admin_login(req, res);
        })));

    api_router->add_router(sf_http_router::make_instance(
        "/group_info"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_group_info(req, res);
        }),
        vector{{"GET"s}}));

    api_router->add_router(sf_http_router::make_instance(
        "/blog"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            user_get_blog(req, res);
        }),
        vector{{"GET"s}}));

    api_router->add_router(sf_http_router::make_instance(
        "/blog_content"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            user_get_content(req, res);
        }),
        vector{{"GET"s}}));

    api_router->add_router(sf_http_router::make_instance(
        "/blog_labels"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            user_get_label(req, res);
        }),
        vector{{"GET"s}}));

    api_router->add_router(sf_http_router::make_instance(
        "/blog_info"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_blog_info(req, res);
        }),
        vector{{"GET"s}}));

    api_router->add_router(sf_http_router::make_instance(
        "/blog_all"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            user_get_all_blog(req, res);
        }),
        vector{{"GET"s}}));

    root_router->add_router(api_router);

    auto admin_api_router = sf_http_part_router::make_instance(
        "/api"s, [this](const sf_http_request &req, sf_http_response &res) {
            return true;
        });

    admin_api_router->add_router(sf_http_router::make_instance(
        "/user_info"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_user_info(req, res);
        }),
        vector{{"GET"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/base_info"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            set_base_info(req, res);
        }),
        vector{{"POST"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/change_password"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            change_password(req, res);
        }),
        vector{{"POST"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/logout"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            logout(req, res);
        }),
        vector{{"*"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/big_group"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            add_big_group(req, res);
        }),
        vector{{"POST"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/sub_group"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            add_sub_group(req, res);
        }),
        vector{{"POST"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/big_group"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            rename_big_group(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/sub_group"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            rename_sub_group(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/big_group"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_big_group(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/sub_group"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_sub_group(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/label"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_label(req, res);
        }),
        vector{{"GET"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/label"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_label(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/label"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            rename_label(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/label"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            add_label(req, res);
        }),
        vector{{"POST"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_list"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_blog_list(req, res);
        }),
        vector{{"GET"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/draft"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            update_draft(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/draft"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_draft(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/draft_list"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_draft_list(req, res);
        }),
        vector{{"GET"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/draft_list"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_draft_list(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            add_blog(req, res);
        }),
        vector{{"POST"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/editor"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            editor(req, res);
        }),
        vector{{"GET"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/draft"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_draft(req, res);
        }),
        vector{{"GET"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/top"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            set_top(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/hide"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            set_hide(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_blog(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_content"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            get_blog_content(req, res);
        }),
        vector{{"GET"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_label"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            add_blog_label(req, res);
        }),
        vector{{"POST"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_label"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_blog_label(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_group"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            update_blog_group(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/top_bat"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            set_top_bat(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/hide_bat"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            set_hide_bat(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_bat"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            delete_blog_bat(req, res);
        }),
        vector{{"DELETE"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_group_bat"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            update_blog_group_bat(req, res);
        }),
        vector{{"PUT"s}}));

    admin_api_router->add_router(sf_http_router::make_instance(
        "/blog_label_bat"s,
        function([this](const sf_http_request &req, sf_http_response &res) {
            add_blog_label_bat(req, res);
        }),
        vector{{"POST"s}}));

    admin_router->add_router(admin_api_router);

    server__->add_router(root_router);
}

bool blog_server::start() { return server__->start(); }

void blog_server::admin_login(const sf_http_request &req,
                              sf_http_response &res) {
    sf_json ret;
    sf_finally f([&ret, &res] { res.set_json(ret); });

    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"name", "password"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        ret["redirect"] = "/html/admin_login.html";
        return;
    }

    auto user_info = database__->check_user(param["name"], param["password"]);

    if (user_info == nullptr) {
        ret["code"] = 2;
        ret["msg"] = "user name or password error";
        ret["redirect"] = "/html/admin_login.html";
        return;
    } else {
        auto session_id = req.get_session_id();
        server__->set_session(session_id, "user", to_json(*user_info));
        ret["code"] = 0;
        ret["msg"] = "ok";
        ret["redirect"] = "/admin/html/manage.html";
    }
}

bool blog_server::admin_check(const sf_http_request &req,
                              sf_http_response &res) {
    auto session_id = req.get_session_id();
    auto session = server__->get_session(session_id);
    if (!session.has("user")) {
        res.redirect("/html/admin_login.html");
        return false;
    }
    return true;
}

void blog_server::admin_root(const sf_http_request &req,
                             sf_http_response &res) {
    auto session_id = req.get_session_id();
    auto session = server__->get_session(session_id);
    if (!session.has("user")) {
        res.redirect("/html/admin_login.html");
    } else {
        res.redirect("/admin/html/manage.html");
    }
}

void blog_server::get_user_info(const sf_http_request &req,
                                sf_http_response &res) {
    auto session_id = req.get_session_id();
    auto session = server__->get_session(session_id);
    sf_json ret;
    ret["code"] = 0;
    ret["data"] = session["user"].clone();
    ret["data"].remove("password");
    res.set_json(ret);
}

void blog_server::set_base_info(const sf_http_request &req,
                                sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"name", "title", "desc"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto session = server__->get_session(req.get_session_id());
    session["user"]["name"] = param["name"];
    admin_user user;
    from_json(session["user"], user);
    database__->update_user_info(user);
    database__->update_blog_info(param["title"], param["desc"]);
    ret["code"] = 0;
}

void blog_server::change_password(const sf_http_request &req,
                                  sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"old_password", "new_password"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
    } else {
        auto session = server__->get_session(req.get_session_id());
        if (static_cast<string>(session["user"]["password"]) ==
            hash_password(param["old_password"])) {
            session["user"]["password"] = hash_password(param["new_password"]);
            admin_user user;
            from_json(session["user"], user);
            database__->update_user_info(user);
            ret["code"] = 0;
        } else {
            ret["code"] = 2;
            ret["msg"] = "old password error";
        }
    }
}

void blog_server::logout(const sf_http_request &req, sf_http_response &res) {
    auto session = server__->get_session(req.get_session_id());
    session.remove("user");
    res.redirect("/html/admin_login.html");
}

void blog_server::get_group_info(const sf_http_request &req,
                                 sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    ret["code"] = 0;
    ret["data"] = get_group_info();
}

sf_json blog_server::get_group_info() {
    auto big_groups = database__->get_all_big_group();
    auto sub_groups = database__->get_all_sub_group();
    auto blog_count = database__->get_sub_group_blog_count();
    sf_json ret;
    ret.convert_to_array();
    for (auto &p : big_groups) {
        sf_json tmp = to_json(p);
        tmp["sub_group"] = sf_json();
        tmp["sub_group"].convert_to_array();
        for (auto &q : sub_groups) {
            if (q.big_group == p.id) {
                sf_json tmp_sub = to_json(q);
                if (blog_count.count(q.id) != 0) {
                    tmp_sub["blog_count"] = blog_count[q.id];
                } else {
                    tmp_sub["blog_count"] = 0;
                }
                tmp["sub_group"].append(tmp_sub);
            }
        }
        ret.append(tmp);
    }
    return ret;
}

void blog_server::add_big_group(const sf_http_request &req,
                                sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"group_name"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    };
    if (database__->check_big_group(param["group_name"]) != nullptr) {
        ret["code"] = 2;
        ret["msg"] = "type name already exists";
        return;
    }

    blog_big_group big_group{-1, param["group_name"]};

    database__->insert_big_group(big_group);
    ret["code"] = 0;
}

void blog_server::add_sub_group(const sf_http_request &req,
                                sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"big_group", "group_name"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto big_group =
        static_cast<int>(sf_string_to_long_double(param["big_group"]));
    if (database__->check_sub_group(big_group, param["group_name"]) !=
        nullptr) {
        ret["code"] = 1;
        ret["msg"] = "type name already exists";
        return;
    }
    blog_sub_group sub_group{-1, big_group, param["group_name"]};
    database__->insert_sub_group(sub_group);
    ret["code"] = 0;
}

void blog_server::delete_big_group(const sf_http_request &req,
                                   sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto big_group = static_cast<int>(sf_string_to_long_double(param["id"]));
    if (database__->get_sub_group_count(big_group) != 0) {
        ret["code"] = 2;
        ret["msg"] = "big type has sub type";
        return;
    }
    database__->delete_big_group(big_group);
    ret["code"] = 0;
}

void blog_server::delete_sub_group(const sf_http_request &req,
                                   sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto sub_group = static_cast<int>(sf_string_to_long_double(param["id"]));
    if (database__->get_sub_group_blog_count(sub_group) != 0) {
        ret["code"] = 2;
        ret["msg"] = "sub type has blogs";
        return;
    }
    database__->delete_sub_group(sub_group);
    ret["code"] = 0;
}

void blog_server::rename_big_group(const sf_http_request &req,
                                   sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id", "new_name"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto big_group = static_cast<int>(sf_string_to_long_double(param["id"]));
    if (database__->get_big_group(big_group) == nullptr) {
        ret["code"] = 2;
        ret["msg"] = "type id not found";
        return;
    }
    if (database__->check_big_group(param["new_name"]) != nullptr) {
        ret["code"] = 3;
        ret["msg"] = "type name already exists";
        return;
    }
    blog_big_group t{big_group, param["new_name"]};
    database__->update_big_group(t);
    ret["code"] = 0;
}

void blog_server::rename_sub_group(const sf_http_request &req,
                                   sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id", "new_name", "big_group"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto big_group =
        static_cast<int>(sf_string_to_long_double(param["big_group"]));
    auto sub_group = static_cast<int>(sf_string_to_long_double(param["id"]));

    if (database__->get_sub_group(sub_group) == nullptr) {
        ret["code"] = 2;
        ret["msg"] = "type id not found";
        return;
    }
    if (database__->check_sub_group(big_group, param["new_name"]) != nullptr) {
        ret["code"] = 3;
        ret["msg"] = "type name already exists";
        return;
    }
    blog_sub_group t{sub_group, big_group, param["new_name"]};
    database__->update_sub_group(t);
    ret["code"] = 0;
}

void blog_server::get_label(const sf_http_request &req, sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto data = database__->get_all_label();
    ret["code"] = 0;
    ret["data"] = to_json(data);
}

void blog_server::add_label(const sf_http_request &req, sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"name"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    if (database__->check_label(param["name"]) != nullptr) {
        ret["code"] = 2;
        ret["msg"] = "label already exists";
        return;
    }
    label lab{-1, param["name"]};
    database__->insert_label(lab);
    ret["code"] = 0;
}

void blog_server::rename_label(const sf_http_request &req,
                               sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id", "name"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto lab_id = static_cast<int>(sf_string_to_long_double(param["id"]));
    if (database__->get_label(lab_id) == nullptr) {
        ret["code"] = 2;
        ret["msg"] = "label not found";
        return;
    }
    if (database__->check_label(param["name"]) != nullptr) {
        ret["code"] = 3;
        ret["msg"] = "label already exists";
        return;
    }
    label lab{lab_id, param["name"]};
    database__->update_label(lab);
    ret["code"] = 0;
}

void blog_server::delete_label(const sf_http_request &req,
                               sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id"})) {
        ret["code"] = 1;
        ret["msg"] = "param name";
        return;
    }
    auto lab_id = static_cast<int>(sf_string_to_long_double(param["id"]));
    if (database__->get_label(lab_id) == nullptr) {
        ret["code"] = 2;
        ret["msg"] = "label not found";
        return;
    }
    database__->delete_label(lab_id);
    ret["code"] = 0;
}

void blog_server::get_blog_list(const sf_http_request &req,
                                sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto blogs = database__->get_all_blog();
    auto blogs_json = to_json(blogs);
    for (int i = 0; i < blogs.size(); ++i) {
        auto sub_group = database__->get_sub_group(blogs[i].sub_group);
        if (sub_group == nullptr) {
            ret["code"] = 1;
            ret["msg"] = "server error";
            return;
        }
        blogs_json[i]["sub_group"] = to_json(*sub_group);
        auto big_group = database__->get_big_group(sub_group->big_group);
        if (big_group == nullptr) {
            ret["code"] = 2;
            ret["msg"] = "server error";
            return;
        }
        blogs_json[i]["big_group"] = to_json(*big_group);
        auto labels = database__->get_blog_labels(blogs[i].id);
        blogs_json[i]["labels"] = to_json(labels);
    }
    ret["code"] = 0;
    ret["data"] = blogs_json;
}

void blog_server::get_draft_list(const sf_http_request &req,
                                 sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    ret["code"] = 0;
    ret["data"] = to_json(database__->get_all_draft());
}

void blog_server::update_draft(const sf_http_request &req,
                               sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id", "title", "content"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto draft_id = static_cast<int>(sf_string_to_long_double(param["id"]));
    draft df{draft_id, param["title"], param["content"]};
    ret["code"] = 0;
    if (df.id == -1) {
        ret["data"] = database__->insert_draft(df);
    } else {
        database__->update_draft(df);
        ret["data"] = df.id;
    }
}

void blog_server::delete_draft(const sf_http_request &req,
                               sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto draft_id = static_cast<int>(sf_string_to_long_double(param["id"]));
    if (database__->get_draft(draft_id) == nullptr) {
        ret["code"] = 2;
        ret["msg"] = "draft not found";
        return;
    }
    database__->delete_draft(draft_id);
    ret["code"] = 0;
}

void blog_server::delete_draft_list(const sf_http_request &req,
                                    sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    vector<int> ids;
    from_json(sf_json::from_string(to_string(req.get_body())), ids);
    for (auto &id : ids) {
        database__->delete_draft(id);
    }
    ret["code"] = 0;
}

void blog_server::add_blog(const sf_http_request &req, sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(
            param, {"blog_id", "draft_id", "sub_group", "title", "content"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto blog_id = static_cast<int>(sf_string_to_long_double(param["blog_id"]));
    auto draft_id =
        static_cast<int>(sf_string_to_long_double(param["draft_id"]));
    auto sub_group =
        static_cast<int>(sf_string_to_long_double(param["sub_group"]));
    ret["code"] = 0;
    if (blog_id == -1) {
        blog b{blog_id,   param["title"], sf_make_time_str(), 0, false,
               sub_group, false};
        blog_id = database__->insert_blog(b);
        blog_content bc{blog_id, param["content"]};
        database__->insert_blog_content(bc);
    } else {
        auto b = database__->get_blog(blog_id);
        if (!b) {
            ret["code"] = 2;
            ret["msg"] = "server error";
            return;
        }
        b->title = param["title"];
        b->sub_group = sub_group;
        database__->update_blog(*b);
        blog_content bc{blog_id, param["content"]};
        database__->update_blog_content(bc);
    }
    if (draft_id != -1) {
        database__->delete_draft(draft_id);
    }
}

void blog_server::get_draft(const sf_http_request &req, sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    string url;
    sf_http_param_t param;
    string frame;
    sf_parse_url(req.get_request_line().url, url, param, frame);

    if (!check_param(param, {"id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto id = static_cast<int>(sf_string_to_long_double(param["id"]));
    auto data = database__->get_draft(id);
    if (!data) {
        ret["code"] = 2;
        ret["msg"] = "draft not found";
        return;
    }
    ret["code"] = 0;
    ret["data"] = to_json(*data);
}

void blog_server::set_top(const sf_http_request &req, sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id", "value"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto id = static_cast<int>(sf_string_to_long_double(param["id"]));
    auto value = static_cast<int>(sf_string_to_long_double(param["value"]));
    auto data = database__->get_blog(id);
    if (!data) {
        ret["code"] = 2;
        ret["msg"] = "blog not found";
        return;
    }
    data->top = value;
    database__->update_blog(*data);
    ret["code"] = 0;
}

void blog_server::set_hide(const sf_http_request &req, sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id", "value"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto id = static_cast<int>(sf_string_to_long_double(param["id"]));
    auto value = static_cast<int>(sf_string_to_long_double(param["value"]));
    auto data = database__->get_blog(id);
    if (!data) {
        ret["code"] = 2;
        ret["msg"] = "blog not found";
        return;
    }
    data->hide = value;
    database__->update_blog(*data);
    ret["code"] = 0;
}

void blog_server::delete_blog(const sf_http_request &req,
                              sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto id = static_cast<int>(sf_string_to_long_double(param["id"]));
    if (!database__->get_blog(id)) {
        ret["code"] = 2;
        ret["msg"] = "blog not found";
        return;
    }
    database__->delete_blog(id);
    ret["code"] = 0;
}

void blog_server::get_blog_content(const sf_http_request &req,
                                   sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    string url;
    sf_http_param_t param;
    string frame;
    sf_parse_url(req.get_request_line().url, url, param, frame);

    if (!check_param(param, {"id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto id = static_cast<int>(sf_string_to_long_double(param["id"]));
    auto data = database__->get_blog_content(id);
    if (!data) {
        ret["code"] = 2;
        ret["msg"] = "blog not found";
        return;
    }
    ret["code"] = 0;
    ret["data"] = to_json(*data);
}

void blog_server::add_blog_label(const sf_http_request &req,
                                 sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_json::from_string(to_string(req.get_body()));

    auto blog_id = param["blog_id"];

    for (int i = 0; i < param["label_ids"].size(); ++i) {
        auto label_id = param["label_ids"][i];
        if (database__->check_blog_label(blog_id, label_id)) {
            ret["code"] = 2;
            ret["msg"] = "label already exists";
            return;
        }
        database__->insert_blog_label({blog_id, label_id});
    }
    ret["code"] = 0;
}

void blog_server::delete_blog_label(const sf_http_request &req,
                                    sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"blog_id", "label_id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto blog_id = static_cast<int>(sf_string_to_long_double(param["blog_id"]));
    auto label_id =
        static_cast<int>(sf_string_to_long_double(param["label_id"]));
    if (!database__->check_blog_label(blog_id, label_id)) {
        ret["code"] = 2;
        ret["msg"] = "label not found";
        return;
    }
    database__->delete_blog_label(blog_id, label_id);
    ret["code"] = 0;
}

void blog_server::editor(const sf_http_request &req, sf_http_response &res) {
    sf_json data;
    bool ok = true;
    sf_finally f([&data, &res, this, &ok] {
        if (ok) {
            sf_json ret;
            ret["data"] = data;
            auto result = env.render_file(
                sf_path_join(blog_config__.static_path, "html/editor.html"s),
                sf_json_to_nlo_json(ret));
            res.set_body(to_byte_array(result));
        }
    });

    string url;
    sf_http_param_t param;
    string frame;
    sf_parse_url(req.get_request_line().url, url, param, frame);

    if (!check_param(param, {"type"})) {
        ok = false;
        res.set_status(401);
        return;
    }

    auto type = static_cast<int>(sf_string_to_long_double(param["type"]));

    data["group_data"] = get_group_info();
    data["big_group_index"] = 0;
    data["sub_group"] = -1;
    data["converter"] = sf_json();
    data["editor"] = sf_json();
    data["blog_id"] = -1;
    data["draft_id"] = -1;
    data["type"] = type;
    data["title"] = "";
    data["auto_save_flag"] = false;
    data["timer"] = sf_json();

    // 添加新文章
    if (type == 0) {
        return;
    } else if (type == 1) {    // 编辑草稿
        if (!check_param(param, {"draft_id"})) {
            ok = false;
            res.set_status(401);
            return;
        }
        data["draft_id"] =
            static_cast<int>(sf_string_to_long_double(param["draft_id"]));
        return;
    } else if (type == 2) {
        if (!check_param(param, {"blog_id"})) {
            ok = false;
            res.set_status(401);
            return;
        }
        auto blog_id =
            static_cast<int>(sf_string_to_long_double(param["blog_id"]));
        auto blog_data = database__->get_blog(blog_id);
        if (!blog_data) {
            ok = false;
            res.set_status(401);
            return;
        }
        auto sub_group = database__->get_sub_group(blog_data->sub_group);
        if (!sub_group) {
            ok = false;
            res.set_status(401);
            return;
        }
        auto big_groups = database__->get_all_big_group();
        auto flag = false;
        for (int k = 0; k < big_groups.size(); ++k) {
            if (big_groups[k].id == sub_group->big_group) {
                data["big_group_index"] = k;
                flag = true;
                break;
            }
        }

        if (!flag) {
            ok = false;
            res.set_status(401);
            return;
        }

        data["blog_id"] = blog_id;
        data["sub_group"] = sub_group->id;
        data["title"] = blog_data->title;
    }
}

void blog_server::update_blog_group(const sf_http_request &req,
                                    sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_parse_param(to_string(req.get_body()));
    if (!check_param(param, {"blog_id", "sub_group"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto blog_id = static_cast<int>(sf_string_to_long_double(param["blog_id"]));
    auto sub_group =
        static_cast<int>(sf_string_to_long_double(param["sub_group"]));
    auto data = database__->get_blog(blog_id);
    if (!data) {
        ret["code"] = 2;
        ret["msg"] = "blog not found";
        return;
    }
    data->sub_group = sub_group;
    database__->update_blog(*data);
    ret["code"] = 0;
}

void blog_server::user_get_blog(const sf_http_request &req,
                                sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    string url;
    sf_http_param_t param;
    string frame;
    sf_parse_url(req.get_request_line().url, url, param, frame);
    if (!check_param(param, {"sub_group"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto sub_group =
        static_cast<int>(sf_string_to_long_double(param["sub_group"]));
    auto blogs = database__->get_top_blogs(sub_group);
    auto data_normal = database__->get_normal_blogs(sub_group);
    blogs.insert(blogs.end(), data_normal.begin(), data_normal.end());
    auto blogs_json = to_json(blogs);
    for (int i = 0; i < blogs.size(); ++i) {
        auto sub_group = database__->get_sub_group(blogs[i].sub_group);
        if (sub_group == nullptr) {
            ret["code"] = 2;
            ret["msg"] = "server error";
            return;
        }
        blogs_json[i]["sub_group"] = to_json(*sub_group);
        auto big_group = database__->get_big_group(sub_group->big_group);
        if (big_group == nullptr) {
            ret["code"] = 3;
            ret["msg"] = "server error";
            return;
        }
        blogs_json[i]["big_group"] = to_json(*big_group);
        auto labels = database__->get_blog_labels(blogs[i].id);
        blogs_json[i]["labels"] = to_json(labels);
    }
    ret["code"] = 0;
    ret["data"] = blogs_json;
}

void blog_server::user_get_all_blog(const sf_http_request &req,
                                    sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto blogs = database__->get_top_blogs();
    auto data_normal = database__->get_normal_blogs();
    blogs.insert(blogs.end(), data_normal.begin(), data_normal.end());
    auto blogs_json = to_json(blogs);
    for (int i = 0; i < blogs.size(); ++i) {
        auto sub_group = database__->get_sub_group(blogs[i].sub_group);
        if (sub_group == nullptr) {
            ret["code"] = 1;
            ret["msg"] = "server error";
            return;
        }
        blogs_json[i]["sub_group"] = to_json(*sub_group);
        auto big_group = database__->get_big_group(sub_group->big_group);
        if (big_group == nullptr) {
            ret["code"] = 2;
            ret["msg"] = "server error";
            return;
        }
        blogs_json[i]["big_group"] = to_json(*big_group);
        auto labels = database__->get_blog_labels(blogs[i].id);
        blogs_json[i]["labels"] = to_json(labels);
    }
    ret["code"] = 0;
    ret["data"] = blogs_json;
}

void blog_server::user_get_content(const sf_http_request &req,
                                   sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    string url;
    sf_http_param_t param;
    string frame;
    sf_parse_url(req.get_request_line().url, url, param, frame);
    if (!check_param(param, {"blog_id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto blog_id = static_cast<int>(sf_string_to_long_double(param["blog_id"]));
    auto data = database__->get_blog_content(blog_id);
    if (!data) {
        ret["code"] = 2;
        ret["msg"] = "blog not found";
        return;
    }
    auto base_info = database__->get_blog(blog_id);
    if (base_info->hide) {
        ret["code"] = 3;
        ret["msg"] = "blog is hiddem";
        return;
    }
    ++base_info->watch_number;
    database__->update_blog(*base_info);
    ret["code"] = 0;
    ret["data"] = to_json(*data);
}

void blog_server::user_get_label(const sf_http_request &req,
                                 sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    string url;
    sf_http_param_t param;
    string frame;
    sf_parse_url(req.get_request_line().url, url, param, frame);
    if (!check_param(param, {"blog_id"})) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    auto blog_id = static_cast<int>(sf_string_to_long_double(param["blog_id"]));
    auto base_info = database__->get_blog(blog_id);
    if (!base_info) {
        ret["code"] = 2;
        ret["msg"] = "blog not found";
        return;
    }
    if (base_info->hide) {
        ret["code"] = 3;
        ret["msg"] = "blog is hiddem";
        return;
    }
    ret["code"] = 0;
    ret["data"] = to_json(database__->get_blog_labels(blog_id));
}

void blog_server::get_blog_info(const sf_http_request &req,
                                sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto data = database__->get_blog_info();
    if (!data) {
        ret["code"] = 1;
        ret["msg"] = "server error";
        return;
    }
    ret["code"] = 0;
    ret["data"] = to_json(*data);
}

void blog_server::set_top_bat(const sf_http_request &req,
                              sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_json::from_string(to_string(req.get_body()));
    if (!param.has("blogs") || !param.has("value")) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    bool value = param["value"];
    for (int i = 0; i < param["blogs"].size(); ++i) {
        auto data = database__->get_blog(param["blogs"][i]);
        if (!data) {
            ret["code"] = 2;
            ret["msg"] = "blog not found";
            return;
        }
        data->top = value;
        database__->update_blog(*data);
    }
    ret["code"] = 0;
}

void blog_server::set_hide_bat(const sf_http_request &req,
                               sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_json::from_string(to_string(req.get_body()));
    if (!param.has("blogs") || !param.has("value")) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    bool value = param["value"];
    for (int i = 0; i < param["blogs"].size(); ++i) {
        auto data = database__->get_blog(param["blogs"][i]);
        if (!data) {
            ret["code"] = 2;
            ret["msg"] = "blog not found";
            return;
        }
        data->hide = value;
        database__->update_blog(*data);
    }
    ret["code"] = 0;
}

void blog_server::delete_blog_bat(const sf_http_request &req,
                                  sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_json::from_string(to_string(req.get_body()));
    for (int i = 0; i < param.size(); ++i) {
        database__->delete_blog(param[i]);
    }
    ret["code"] = 0;
}

void blog_server::update_blog_group_bat(const sf_http_request &req,
                                        sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_json::from_string(to_string(req.get_body()));
    if (!param.has("blogs") || !param.has("sub_group")) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    int value = param["sub_group"];
    for (int i = 0; i < param["blogs"].size(); ++i) {
        auto data = database__->get_blog(param["blogs"][i]);
        if (!data) {
            ret["code"] = 2;
            ret["msg"] = "blog not found";
            return;
        }
        data->sub_group = value;
        database__->update_blog(*data);
    }
    ret["code"] = 0;
}

void blog_server::add_blog_label_bat(const sf_http_request &req,
                                     sf_http_response &res) {
    sf_json ret;
    sf_finally f([&res, &ret] { res.set_json(ret); });
    auto param = sf_json::from_string(to_string(req.get_body()));
    if (!param.has("blogs") || !param.has("labels")) {
        ret["code"] = 1;
        ret["msg"] = "param error";
        return;
    }
    for (int i = 0; i < param["blogs"].size(); ++i) {
        for (int j = 0; j < param["labels"].size(); ++j) {
            auto data = database__->check_blog_label(param["blogs"][i],
                                                     param["labels"][j]);
            if (data) {
                continue;
            }
            database__->insert_blog_label(
                {param["blogs"][i], param["labels"][j]});
        }
    }
    ret["code"] = 0;
}