#ifndef ACCOUNTS64_H
#define ACCOUNTS64_H

#include <stdint.h>

#define MAX_USERS 10
#define MAX_USERNAME_LEN 16
#define MAX_PASSWORD_LEN 32
#define PASSWORD_HASH_LEN 64

typedef enum {
    USER_LEVEL_GUEST = 0,
    USER_LEVEL_USER = 1,
    USER_LEVEL_ADMIN = 2,
    USER_LEVEL_ROOT = 3
} UserLevel;

typedef struct {
    char username[MAX_USERNAME_LEN];
    char password_hash[PASSWORD_HASH_LEN];
    UserLevel level;
    uint8_t is_active;
    uint64_t created_time;
    uint64_t last_login;
    uint32_t login_count;
    char home_dir[64];
} User;

typedef struct {
    User users[MAX_USERS];
    int user_count;
    int current_user_id;
    uint8_t is_logged_in;
} AccountSystem;

// Account system functions
void accounts_init(void);
int accounts_create_user(const char* username, const char* password, UserLevel level);
int accounts_login(const char* username, const char* password);
void accounts_logout(void);
int accounts_delete_user(const char* username);
int accounts_change_password(const char* old_password, const char* new_password);
int accounts_is_logged_in(void);
const char* accounts_get_current_username(void);
UserLevel accounts_get_current_level(void);
int accounts_list_users(char output[][128], int max_lines);

// Permission checking
int accounts_has_permission(UserLevel required_level);
const char* accounts_level_to_string(UserLevel level);

// Password utilities
void accounts_hash_password(const char* password, char* hash_out);
int accounts_verify_password(const char* password, const char* hash);

#endif // ACCOUNTS64_H