#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <stdarg.h>
#include <windows.h>
#include <dwmapi.h>


#ifdef ENABLE_ACCENT_REPORT
#include "winrt_ui_settings.h"
#endif


// definitions for DwmSetWindowAttribute
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

#ifndef DWMWA_USE_MICA
#define DWMWA_USE_MICA 1029
#endif

#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif


uintptr_t _beginthreadex( // NATIVE CODE
   void *security,
   unsigned stack_size,
   unsigned ( __stdcall *start_address )( void * ),
   void *arglist,
   unsigned initflag,
   unsigned *thrdaddr
);


#define MAX_CLASS_SIZE 512
#define BUFFER_SIZE 512

#define CMD_CONFIG "config"
#define CMD_THEME "theme"
#define CMD_EXIT "exit"
#define CMD_ACCENT "accent"

#define RESPONSE_OK "ok"
#define RESPONSE_ERROR "error"

#define BROADCAST_THEMECHANGE "themechange"
#define BROADCAST_ERROR "error"
#define BROADCAST_READY "ready"

#define WIN10_BUILD_NUMBER 18362
#define WIN11_BUILD_NUMBER 22000
#define WIN11_SYSTEMBACKDROP_SUPPORTED_BUILD_NUMBER 22621


typedef enum {
    BACKDROP_DEFAULT,
    BACKDROP_NONE,
    BACKDROP_MICA,
    BACKDROP_ACRYLIC,
    BACKDROP_TABBED,
    BACKDROP_MAX,
} window_backdrop_e;

typedef enum {
    CONFIG_DARK_MODE = 1,
    CONFIG_EXTEND_BORDER = 2,
    CONFIG_BACKDROP_TYPE = 4
} config_changed_e;

typedef struct window_config_s {
    DWORD pid;
    HWND window;
    HKEY regkey;
#ifdef ENABLE_ACCENT_REPORT
    winrt_ui_settings_t *ui_settings;
#endif
    int dark_mode, extend_border;
    window_backdrop_e backdrop_type;
    config_changed_e mask;
    CONDITION_VARIABLE config_changed;
    CRITICAL_SECTION mutex;
    char class[MAX_CLASS_SIZE];
} window_config_t;


/**
 * This program communicates via newline (\n) terminated messages.
 * The message should not exceed 512 bytes in size, including the newline.
 * The message follows a specific format:
 * serial " " type " " response?
 * 
 * serial refers to an arbitary value that is sent via the client
 * that is used to identify the response.
 * This value is preferrably a number, but it can be anything that
 * does not contains a space (" ") character.
 * Note that -1 is reserved for broadcasts.
 * 
 * type is the type of message or response.
 * 
 * Anything after type is intepreted as the content.
 * This content is delimited by a single space (" ") and can be optional.
 * "-1 error " is a valid message.
 * 
 * A response have the format of:
 * serial " " status " " message
 * 
 * serial is the serial of the incoming message,
 * while status can be "ok" or "error" to denote a successful or failed operation
 * respectively.
 * message is optional, and in case of errors, will be the error message.
 */
static void log_response(const char *serial, const char *type, const char *fmt, ...) {
    va_list ap;
    printf("%s %s ", serial, type);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putc('\n', stdout);
}


#define log_broadcast(type, fmt, ...) (printf("-1 " type " " fmt "\n", __VA_ARGS__))
#define log_error(fmt, ...) (log_broadcast(BROADCAST_ERROR, fmt, __VA_ARGS__))


static int parse_message(char *msg, char **serial, char **type, char **content) {
    char *p, *_serial, *_type, *_content;
    p = _serial = _type = _content = NULL;

#define NEXT_TOKEN() do { \
        p = strchr(msg, ' '); \
        if (!p) return 0; \
        *p = '\0'; \
    } while (0)
#define NEXT_TOKEN_END() msg = p + 1

    // find the serial
    NEXT_TOKEN();
    _serial = msg;
    NEXT_TOKEN_END();
    // find the type
    NEXT_TOKEN();
    _type = msg;
    NEXT_TOKEN_END();
    // content is the rest of the string
    _content = msg;

    *serial = _serial;
    *type = _type;
    *content = _content;

    return 1;
#undef NEXT_TOKEN
#undef NEXT_TOKEN_END
}


static void log_win32_response(const char *serial, const char *type, const char *function_name, DWORD rc) {
    LPSTR msg = NULL;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER
                    | FORMAT_MESSAGE_FROM_SYSTEM
                    | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL,
                    rc,
                    MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (LPSTR) &msg,
                    0,
                    NULL);
    log_response(serial, type, "%s: %s", function_name, msg ? msg : "unknown error");
    LocalFree(msg);
}


#define log_win32_error(name, rc) (log_win32_response("-1", BROADCAST_ERROR, name, rc))


static LSTATUS is_dark_mode(HKEY regkey, int *is_dark) {
    LSTATUS rc;
    HKEY key = NULL;
    DWORD type, value, size = 4;

    rc = RegQueryValueEx(regkey,
                            "AppsUseLightTheme",
                            NULL,
                            &type,
                            (LPBYTE)&value, &size);

    if (rc != ERROR_SUCCESS)
        return rc;
    if (type != REG_DWORD)
        return ERROR_INVALID_PARAMETER;

    *is_dark = !value;
    return ERROR_SUCCESS;
}


static unsigned __stdcall theme_monitor_proc(void *ud) {
    int value;
    HANDLE ev;
    LRESULT rc;
    window_config_t *config = (window_config_t *) ud;

    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!ev) {
        log_win32_error("CreateEventA", GetLastError());
        return 0;
    }

    for (;;) {
        // we must call LeaveCriticalSection when we decided to break
        // Get an event so that we can unlock the mutex and wait
        EnterCriticalSection(&config->mutex);

        if (!config->regkey) {
            LeaveCriticalSection(&config->mutex);
            break;
        }

        rc = RegNotifyChangeKeyValue(config->regkey,
                                        FALSE,
                                        REG_NOTIFY_CHANGE_LAST_SET,
                                        ev,
                                        TRUE);
        if (rc != ERROR_SUCCESS) {
            log_win32_error("RegNotifyChangeKeyValue", rc);
            LeaveCriticalSection(&config->mutex);
            break;
        }

        LeaveCriticalSection(&config->mutex);

        // wait for the mutex
        WaitForSingleObject(ev, INFINITE);

        // read the config
        EnterCriticalSection(&config->mutex);

        if (!config->regkey) {
            LeaveCriticalSection(&config->mutex);
            break;
        }

        rc = is_dark_mode(config->regkey, &value);
        if (rc != ERROR_SUCCESS) {
            log_win32_error("RegQueryValueEx", rc);
            LeaveCriticalSection(&config->mutex);
            break;
        }

        if (value != config->dark_mode) {
            config->dark_mode = value;
            config->mask |= CONFIG_DARK_MODE;
            log_broadcast(BROADCAST_THEMECHANGE, "%d", config->dark_mode);
            WakeConditionVariable(&config->config_changed);
        }

        LeaveCriticalSection(&config->mutex);
    }
    return 0;
}


static unsigned __stdcall config_change_proc(void *ud) {
    HRESULT hr;
    OSVERSIONINFOEXA version;
    window_config_t *config = (window_config_t *) ud;

    // get the OS version so we know how to set the correct attribute later
    version.dwOSVersionInfoSize = sizeof(version);
    if (!GetVersionExA((LPOSVERSIONINFOA) &version)) {
        log_win32_error("GetVersionExA", GetLastError());
        return 0;
    }

    if (version.dwBuildNumber < WIN10_BUILD_NUMBER) {
        log_error("windows build unsupported: %ld", version.dwBuildNumber);
        return 0;
    }

    for (;;) {
        MARGINS m = { 0 };
        DWORD value;

        // once again, we must call LeaveCriticalSection when breaking!
        EnterCriticalSection(&config->mutex);

        while (config->window && !config->mask)
            SleepConditionVariableCS(&config->config_changed, &config->mutex, INFINITE);

        if (!config->window) {
            LeaveCriticalSection(&config->mutex);
            break;
        }

        // extend the frame
        if (config->mask & CONFIG_EXTEND_BORDER) {
            if (config->extend_border)
                m.cxLeftWidth = m.cxRightWidth = m.cyBottomHeight = m.cyTopHeight = -1;
            hr = DwmExtendFrameIntoClientArea(config->window, &m);
            if (FAILED(hr)) {
                log_win32_error("DwmExtendFrameIntoClientArea", HRESULT_CODE(hr));
                LeaveCriticalSection(&config->mutex);
                break;
            }
        }

        // set window light/dark theme
        if (config->mask & CONFIG_DARK_MODE) {
            value = config->dark_mode;
            hr = DwmSetWindowAttribute(config->window,
                                        DWMWA_USE_IMMERSIVE_DARK_MODE,
                                        &value,
                                        sizeof(DWORD));
            if (FAILED(hr)) {
                log_win32_error("DwmSetWindowAttribute(DWMMA_USE_IMMERSIVE_DARK_MODE)", HRESULT_CODE(hr));
                LeaveCriticalSection(&config->mutex);
                break;
            }
        }

        // set window backdrop
        if (config->mask & CONFIG_BACKDROP_TYPE) {
            if (version.dwBuildNumber >= WIN11_SYSTEMBACKDROP_SUPPORTED_BUILD_NUMBER) {
                value = config->backdrop_type;
                hr = DwmSetWindowAttribute(config->window,
                                            DWMWA_SYSTEMBACKDROP_TYPE,
                                            &value,
                                            sizeof(DWORD));
                if (FAILED(hr)) {
                    log_win32_error("DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)", HRESULT_CODE(hr));
                    LeaveCriticalSection(&config->mutex);
                    break;
                }
            } else if (version.dwBuildNumber < WIN11_SYSTEMBACKDROP_SUPPORTED_BUILD_NUMBER
                        && config->backdrop_type == BACKDROP_MICA) {
                // on older versions we should use another method that only supports mica
                value = 1;
                hr = DwmSetWindowAttribute(config->window,
                                            DWMWA_USE_MICA,
                                            &value,
                                            sizeof(DWORD));
                if (FAILED(hr)) {
                    log_win32_error("DwmSetWindowAttribute(DWMWA_USE_MICA)", HRESULT_CODE(hr));
                    LeaveCriticalSection(&config->mutex);
                    break;
                }
            } else {
                log_error("unsupported windows version for backdrop type %d", config->backdrop_type);
            }
        }

        // clear config mask
        config->mask = 0;
        LeaveCriticalSection(&config->mutex);
    }
    return 0;
}


static unsigned __stdcall read_input_proc(void *ud) {
    char buffer[BUFFER_SIZE];
    window_config_t *config = (window_config_t *) ud;

    while (fgets(buffer, sizeof(buffer), stdin)) {
        char *serial, *type, *content, *p;

        // if string ends with newline, remove it
        p = strrchr(buffer, '\n');
        if (p)
            *p = '\0';

        EnterCriticalSection(&config->mutex);
        // check if window is valid before we continue processing
        if (!config->window || !IsWindow(config->window)) {
            LeaveCriticalSection(&config->mutex);
            break;
        }

        if (!parse_message(buffer, &serial, &type, &content)) {
            log_error("invalid command: \"%s\"", buffer);
            LeaveCriticalSection(&config->mutex);
            continue;
        }

        if (strcmp(type, CMD_CONFIG) == 0) {
            int value;

            if (strlen(content) != 2) {
                log_response(serial, RESPONSE_ERROR, "invalid length: %d", strlen(content));
                LeaveCriticalSection(&config->mutex);
                continue;
            }

            // extend border
            value = content[0] - '0';
            if (value != config->extend_border) {
                config->extend_border = !!value;
                config->mask |= CONFIG_EXTEND_BORDER;
            }

            // backdrop type
            value = content[1] - '0';
            if (value < 0 || value >= BACKDROP_MAX) {
                log_response(serial, RESPONSE_ERROR, "invalid backdrop type: %c", content[1]);
                LeaveCriticalSection(&config->mutex);
                continue;
            }
            if (value != config->backdrop_type) {
                config->backdrop_type = value;
                config->mask |= CONFIG_BACKDROP_TYPE;
            }

            WakeConditionVariable(&config->config_changed);
            log_response(serial, RESPONSE_OK, "");
        } else if (strcmp(type, CMD_THEME) == 0) {
            int value;
            LRESULT rc;

            rc = is_dark_mode(config->regkey, &value);
            if (rc != ERROR_SUCCESS) {
                log_win32_response(serial, RESPONSE_ERROR, "is_dark_mode", rc);
                LeaveCriticalSection(&config->mutex);
                break;
            }

            log_response(serial, type, "%d", value);
#ifdef ENABLE_ACCENT_REPORT
        } else if (strcmp(type, CMD_ACCENT) == 0) {
            int type;
            HRESULT hr;
            uint32_t value = 0;

            if (strlen(content) != 1) {
                log_response(serial, RESPONSE_ERROR, "invalid length: %d", strlen(content));
                LeaveCriticalSection(&config->mutex);
                continue;
            }

            type = content[0] - '0';
            if (type > COLOR_TYPE_MIN && type < COLOR_TYPE_MAX) {
                hr = winrt_ui_settings_get_color(config->ui_settings, type, &value);
                if (FAILED(hr)) {
                    log_win32_response(serial, RESPONSE_ERROR, "winrt_ui_settings_get_color", HRESULT_CODE(hr));
                    LeaveCriticalSection(&config->mutex);
                    break;
                }
                log_response(serial, RESPONSE_OK, "%u", value);
            } else {
                log_response(serial, RESPONSE_ERROR, "invalid type: %c", content[0]);
            }
#endif
        } else if (strcmp(type, CMD_EXIT) == 0) {
            log_response(serial, RESPONSE_OK, "");
            LeaveCriticalSection(&config->mutex);
            break;
        } else {
            log_response(serial, RESPONSE_ERROR, "invalid command: \"%s\"", type);
        }
        LeaveCriticalSection(&config->mutex);
    }
    return 0;
}


BOOL CALLBACK enum_window_proc(HWND hwnd, LPARAM lparam) {  
    DWORD pid;
    char buffer[MAX_CLASS_SIZE];
    window_config_t *target = (window_config_t *) lparam;
    if (!GetWindowThreadProcessId(hwnd, &pid)
        || !GetClassNameA(hwnd, buffer, MAX_CLASS_SIZE))
        return FALSE;
    if (pid == target->pid && strcmp(buffer, target->class) == 0) {
        target->window = hwnd;
        return FALSE;
    }
    return TRUE;
}


int main(int argc, char **argv) {
    DWORD rc;
    window_config_t config = { 0 };
    HANDLE thread_handles[3] = { INVALID_HANDLE_VALUE };

#ifdef ENABLE_ACCENT_REPORT
    HRESULT hr;
    hr = winrt_initialize();
    if (FAILED(hr)) {
        log_win32_error("winrt_initialize", HRESULT_CODE(hr));
        return 0;
    }
#endif

    if (argc != 3) {
        log_error("invalid number of arguments: %d", argc);
        return 0;
    }

    // find the current window
    config.pid = strtol(argv[1], NULL, 10);
    snprintf(config.class, MAX_CLASS_SIZE, "%s", argv[2]);
    InitializeCriticalSection(&config.mutex);
    InitializeConditionVariable(&config.config_changed);
#ifdef ENABLE_ACCENT_REPORT
    hr = winrt_ui_settings_new(&config.ui_settings);
    if (FAILED(hr)) {
        log_win32_error("winrt_ui_settings_new", HRESULT_CODE(hr));
        goto exit;
    }
#endif
    if (!EnumWindows(&enum_window_proc,(LPARAM) &config) && GetLastError() != ERROR_SUCCESS) {
        log_win32_error("EnumWindows", GetLastError());
        goto exit;
    }
    if (!config.window) {
        log_error("cannot find window class %s owned by %ld", config.class, config.pid);
        goto exit;
    }

    rc = RegOpenKeyExA(HKEY_CURRENT_USER,
                        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        0,
                        KEY_READ | KEY_NOTIFY,
                        &config.regkey);
    if (rc != ERROR_SUCCESS) {
        log_win32_error("RegOpenKeyExA", rc);
        goto exit;
    }

    rc = is_dark_mode(config.regkey, &config.dark_mode);
    if (rc != ERROR_SUCCESS) {
        log_win32_error("RegQueryValueExA", rc);
        goto exit;
    }
    config.mask |= CONFIG_DARK_MODE;
    WakeConditionVariable(&config.config_changed);

    thread_handles[0] = (HANDLE) _beginthreadex(NULL, 0, &theme_monitor_proc, &config, 0, NULL);
    thread_handles[1] = (HANDLE) _beginthreadex(NULL, 0, &config_change_proc, &config, 0, NULL);
    thread_handles[2] = (HANDLE) _beginthreadex(NULL, 0, &read_input_proc, &config, 0, NULL);

    for (int i = 0;i < sizeof(thread_handles) / sizeof(*thread_handles); i++) {
        if (thread_handles[i] == INVALID_HANDLE_VALUE) {
            log_error("cannot create threads: %s", strerror(errno));
            goto exit;
        }
    }

    log_broadcast(BROADCAST_READY, "%s", "");

    rc = WaitForMultipleObjects(sizeof(thread_handles) / sizeof(*thread_handles),
                                thread_handles,
                                FALSE,
                                INFINITE);
    if (rc >= WAIT_OBJECT_0 && rc <= WAIT_OBJECT_0 + 2) {
        // close the regkey if any of the threads failed
        EnterCriticalSection(&config.mutex);
        if (config.regkey)
            RegCloseKey(config.regkey);
        config.regkey = NULL;
        config.window = NULL;
        WakeConditionVariable(&config.config_changed);
        // workaround: cancel IO in the input thread so it can end immediately
        CancelSynchronousIo(thread_handles[2]);
        LeaveCriticalSection(&config.mutex);


        // wait for the rest of the threads to quit
        WaitForMultipleObjects(sizeof(thread_handles) / sizeof(*thread_handles),
                                thread_handles,
                                TRUE,
                                INFINITE);
    } else {
        log_win32_error("WaitForMultipleObjects", GetLastError());
        goto exit;
    }

exit:
    for (int i = 0; i < sizeof(thread_handles) / sizeof(*thread_handles); i++) {
        if (thread_handles[i] != INVALID_HANDLE_VALUE)
            CloseHandle(thread_handles[i]);
    }
    if (config.regkey)
        RegCloseKey(config.regkey);
    DeleteCriticalSection(&config.mutex);
#ifdef ENABLE_ACCENT_REPORT
    if (config.ui_settings)
        winrt_ui_settings_free(config.ui_settings);
    winrt_deinitialize();
#endif
    return 0;
}