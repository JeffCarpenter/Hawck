/* =====================================================================================
 * Macro daemon.
 *
 * Copyright (C) 2018 Jonas Møller (no) <jonas.moeller2@protonmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS     OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * =====================================================================================
 */

#include <thread>
#include <iostream>

extern "C" {
    #include <libnotify/notify.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <syslog.h>
}

#include "Daemon.hpp"
#include "MacroDaemon.hpp"
#include "LuaUtils.hpp"
#include "utils.hpp"
#include "Permissions.hpp"
#include "LuaConfig.hpp"
#include "XDG.hpp"
#include "KBDB.hpp"

using namespace Lua;
using namespace Permissions;
using namespace std;

static const char *event_str[EV_CNT];

static bool macrod_main_loop_running = true;

static inline void initEventStrs()
{
    event_str[EV_SYN      ] = "SYN"       ;
    event_str[EV_KEY      ] = "KEY"       ;
    event_str[EV_REL      ] = "REL"       ;
    event_str[EV_ABS      ] = "ABS"       ;
    event_str[EV_MSC      ] = "MSC"       ;
    event_str[EV_SW       ] = "SW"        ;
    event_str[EV_LED      ] = "LED"       ;
    event_str[EV_SND      ] = "SND"       ;
    event_str[EV_REP      ] = "REP"       ;
    event_str[EV_FF       ] = "FF"        ;
    event_str[EV_PWR      ] = "PWR"       ;
    event_str[EV_FF_STATUS] = "FF_STATUS" ;
    event_str[EV_MAX      ] = "MAX"       ;
}

MacroDaemon::MacroDaemon()
    : kbd_srv("/var/lib/hawck-input/kbd.sock"),
      xdg("hawck")
{
    notify_on_err = true;
    stop_on_err = false;
    eval_keydown = true;
    eval_keyup = true;
    eval_repeat = true;
    disabled = false;

    auto [grp, grpbuf] = getgroup("hawck-input-share");
    (void) grpbuf;
    if (chown("/var/lib/hawck-input/kbd.sock", getuid(), grp->gr_gid) == -1)
        throw SystemError("Unable to chown kbd.sock: ", errno);
    if (chmod("/var/lib/hawck-input/kbd.sock", 0660) == -1)
        throw SystemError("Unable to chmod kbd.sock: ", errno);
    initEventStrs();
    notify_init("Hawck");
    xdg.mkpath(0700, XDG_DATA_HOME, "scripts-enabled");
    initScriptDir(xdg.path(XDG_DATA_HOME, "scripts-enabled"));
}

void MacroDaemon::getConnection() {
    if (kbd_com)
        delete kbd_com;
    syslog(LOG_INFO, "Listening for a connection ...");

    // Keep looping around until we get a connection.
    for (;;) {
        try {
            int fd = kbd_srv.accept();
            kbd_com = new UNIXSocket<KBDAction>(fd);
            syslog(LOG_INFO, "Got a connection");
            break;
        } catch (SocketError &e) {
            cout << "MacroDaemon accept() error: " << e.what() << endl;
        }
        // Wait for 0.1 seconds
        usleep(100000);
    }

    remote_udev.setConnection(kbd_com);
}

MacroDaemon::~MacroDaemon() {
    for (auto &[_, s] : scripts) {
        (void) _;
        delete s;
    }
}

void MacroDaemon::initScriptDir(const std::string &dir_path) {
    auto dir = shared_ptr<DIR>(opendir(dir_path.c_str()), &closedir);
    struct dirent *entry;
    while ((entry = readdir(dir.get()))) {
        stringstream path_ss;
        path_ss << dir_path << "/" << entry->d_name;
        string path = path_ss.str();

        // Attempt to load the script:
        try {
            loadScript(path);
        } catch (exception &e) {
            cout << "Error: " << e.what() << endl;
        }
    }
    auto files = mkuniq(fsw.addFrom(dir_path));
}

void MacroDaemon::loadScript(const std::string &rel_path) {
    string bn = pathBasename(rel_path);
    if (!goodLuaFilename(bn)) {
        cout << "Wrong filename, not loading: " << bn << endl;
        return;
    }

    auto chdir = xdg.cd(XDG_DATA_HOME, "scripts");

    char *rpath_chars = realpath(rel_path.c_str(), nullptr);
    if (rpath_chars == nullptr)
        throw SystemError("Error in realpath: ", errno);
    string path(rpath_chars);
    free(rpath_chars);

    cout << "Preparing to load script: " << rel_path << endl;

    if (!checkFile(path, "frwxr-xr-x ~:*"))
        return;

    auto sc = mkuniq(new Script());
    sc->call("require", "init");
    sc->open(&remote_udev, "udev");
    sc->from(path);

    string name = pathBasename(rel_path);

    if (scripts.find(name) != scripts.end()) {
        // Script already loaded, reload it
        delete scripts[name];
        scripts.erase(name);
    }

    cout << "Loaded script: " << name << endl;
    scripts[name] = sc.release();
}

void MacroDaemon::unloadScript(const std::string &rel_path) {
    string name = pathBasename(rel_path);
    if (scripts.find(name) != scripts.end()) {
        cout << "delete scripts[" << name << "]" << endl;
        delete scripts[name];
        scripts.erase(name);
    }
}

struct script_error_info {
    lua_Debug ar;
    char path[];
};

void MacroDaemon::notify(string title, string msg) {
    // AFAIK you don't have to free the memory manually, but I could be wrong.
    NotifyNotification *n = notify_notification_new(title.c_str(), msg.c_str(), "hawck");
    notify_notification_set_timeout(n, 12000);
    notify_notification_set_urgency(n, NOTIFY_URGENCY_CRITICAL);
    notify_notification_set_app_name(n, "Hawck");
    syslog(LOG_INFO, "%s", msg.c_str());

    if (!notify_notification_show(n, nullptr)) {
        syslog(LOG_INFO, "Notifications cannot be shown.");
    }
}

static void handleSigPipe(int) {
}

#if 0
static void handleSigTerm(int) {
    macrod_main_loop_running = false;
}
#endif

bool MacroDaemon::runScript(Lua::Script *sc, const struct input_event &ev, string kbd_hid) {
    static bool had_stack_leak_warning = false;
    bool repeat = true;

    try {
        auto [succ] = sc->call<bool>("__match",
                                     (int)ev.value,
                                     (int)ev.code,
                                     (int)ev.type,
                                     kbd_hid);
        if (lua_gettop(sc->getL()) != 0) {
            if (!had_stack_leak_warning) {
                syslog(LOG_WARNING,
                       "API misuse causing Lua stack leak of %d elements.",
                       lua_gettop(sc->getL()));
                // Don't spam system logs:
                had_stack_leak_warning = true;
            }
            lua_settop(sc->getL(), 0);
        }
        repeat = !succ;
    } catch (const LuaError &e) {
        if (stop_on_err)
            sc->setEnabled(false);
        std::string report = e.fmtReport();
        if (notify_on_err)
            notify("Lua error", report);
        syslog(LOG_ERR, "LUA:%s", report.c_str());
        repeat = true;
    }

    return repeat;
}

void MacroDaemon::reloadAll() {
    lock_guard<mutex> lock(scripts_mtx);
    auto chdir = xdg.cd(XDG_DATA_HOME, "scripts");
    for (auto &[_, sc] : scripts) {
        (void) _;
        try {
            sc->setEnabled(true);
            sc->reset();
            sc->call("require", "init");
            sc->open(&remote_udev, "udev");
            sc->reload();
        } catch (const LuaError& e) {
            syslog(LOG_ERR, "Error when reloading script: %s", e.what());
            sc->setEnabled(false);
        }
    }
}

void MacroDaemon::run() {
    syslog(LOG_INFO, "Setting up MacroDaemon ...");

    macrod_main_loop_running = true;
    // FIXME: Need to handle socket timeouts before I can use this SIGTERM handler.
    //signal(SIGTERM, handleSigTerm);

    signal(SIGPIPE, handleSigPipe);

    KBDAction action;
    struct input_event &ev = action.ev;

    // Setup/start LuaConfig
    xdg.mkfifo("lua-comm.fifo");
    xdg.mkfifo("json-comm.fifo");

    LuaConfig conf(xdg.path(XDG_RUNTIME_DIR, "lua-comm.fifo"),
                   xdg.path(XDG_RUNTIME_DIR, "json-comm.fifo"),
                   xdg.path(XDG_DATA_HOME, "cfg.lua"));
    conf.addOption("notify_on_err", &notify_on_err);
    conf.addOption("stop_on_err", &stop_on_err);
    conf.addOption("eval_keydown", &eval_keydown);
    conf.addOption("eval_keyup", &eval_keyup);
    conf.addOption("eval_repeat", &eval_repeat);
    conf.addOption("disabled", &disabled);
    conf.addOption<string>("keymap", [this](string) {reloadAll();});
    conf.start();

    fsw.setWatchDirs(true);
    fsw.setAutoAdd(false);
    fsw.asyncWatch([this](FSEvent &ev) {
        lock_guard<mutex> lock(scripts_mtx);
        try {
            if (ev.mask & IN_DELETE) {
                cout << "Deleting script: " << ev.name << endl;
                unloadScript(ev.name);
            } else if (ev.mask & IN_MODIFY) {
                cout << "Reloading script: " << ev.path << endl;
                if (!S_ISDIR(ev.stbuf.st_mode)) {
                    unloadScript(pathBasename(ev.path));
                    loadScript(ev.path);
                }
            } else if (ev.mask & IN_CREATE) {
                loadScript(ev.path);
            } else {
                cout << "Received unhandled event" << endl;
            }
        } catch (exception &e) {
            cout << e.what() << endl;
        }
        return true;
    });

    KBDB kbdb;

    getConnection();

    syslog(LOG_INFO, "Starting main loop");

    while (macrod_main_loop_running) {
        try {
            bool repeat = true;

            kbd_com->recv(&action);
            string kbd_hid = kbdb.getID(&action.dev_id);

            if (!( (!eval_keydown && ev.value == 1) ||
                   (!eval_keyup && ev.value == 0) ) && !disabled)
            {
                lock_guard<mutex> lock(scripts_mtx);
                // Look for a script match.
                for (auto &[_, sc] : scripts) {
                    (void) _;
                    if (sc->isEnabled() && !(repeat = runScript(sc, ev, kbd_hid)))
                        break;
                }
            }

            if (repeat)
                remote_udev.emit(&ev);

            remote_udev.done();
        } catch (const SocketError& e) {
            // Reset connection
            syslog(LOG_ERR, "Socket error: %s", e.what());
            notify("Socket error", "Connection to InputD timed out, reconnecting ...");
            getConnection();
        }
    }

    syslog(LOG_INFO, "macrod exiting ...");
}
