// Auto-generated
#include "cmd.h"

void init_cmds() {
    extern void fs_init(void);
    fs_init();
    extern void cmd_cat(char* args);
    register_cmd("cat", cmd_cat);
    extern void cmd_cd(char* args);
    register_cmd("cd", cmd_cd);
    extern void cmd_clear(char* args);
    register_cmd("clear", cmd_clear);
    extern void cmd_echo(char* args);
    register_cmd("echo", cmd_echo);
    extern void cmd_editor(char* args);
    register_cmd("editor", cmd_editor);
    extern void cmd_help(char* args);
    register_cmd("help", cmd_help);
    extern void cmd_ls(char* args);
    register_cmd("ls", cmd_ls);
    extern void cmd_mkdir(char* args);
    register_cmd("mkdir", cmd_mkdir);
    extern void cmd_pwd(char* args);
    register_cmd("pwd", cmd_pwd);
    extern void cmd_reboot(char* args);
    register_cmd("reboot", cmd_reboot);
    extern void cmd_rm(char* args);
    register_cmd("rm", cmd_rm);
    extern void cmd_rmdir(char* args);
    register_cmd("rmdir", cmd_rmdir);
    extern void cmd_touch(char* args);
    register_cmd("touch", cmd_touch);
}
