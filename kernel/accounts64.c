#include "accounts64.h"
#include "commands64.h"
#include <stddef.h>

static AccountSystem account_system;

// Simple hash function for passwords (NOT cryptographically secure - for demo only!)
static uint64_t simple_hash(const char* str) {
    uint64_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

void accounts_hash_password(const char* password, char* hash_out) {
    // Simple hash - in production use bcrypt/scrypt!
    uint64_t h1 = simple_hash(password);
    uint64_t h2 = simple_hash(password + (str_len(password) / 2));
    
    // Convert to hex string
    const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        hash_out[i] = hex[(h1 >> (60 - i * 4)) & 0xF];
    }
    for (int i = 0; i < 16; i++) {
        hash_out[16 + i] = hex[(h2 >> (60 - i * 4)) & 0xF];
    }
    hash_out[32] = '\0';
}

int accounts_verify_password(const char* password, const char* hash) {
    char computed_hash[PASSWORD_HASH_LEN];
    accounts_hash_password(password, computed_hash);
    return str_cmp(computed_hash, hash) == 0;
}

void accounts_init(void) {
    account_system.user_count = 0;
    account_system.current_user_id = -1;
    account_system.is_logged_in = 0;
    
    // Initialize all users as inactive
    for (int i = 0; i < MAX_USERS; i++) {
        account_system.users[i].is_active = 0;
        account_system.users[i].username[0] = '\0';
    }
    
    // Create default root user (password: "root")
    accounts_create_user("root", "root", USER_LEVEL_ROOT);
    
    // Create a guest user (password: "guest")
    accounts_create_user("guest", "guest", USER_LEVEL_GUEST);
    
    // Auto-login as root
    accounts_login("root", "root");
}

int accounts_create_user(const char* username, const char* password, UserLevel level) {
    // Check if we have space
    if (account_system.user_count >= MAX_USERS) {
        return 0;
    }
    
    // Validate username
    if (str_len(username) == 0 || str_len(username) >= MAX_USERNAME_LEN) {
        return 0;
    }
    
    // Check if username already exists
    for (int i = 0; i < MAX_USERS; i++) {
        if (account_system.users[i].is_active && 
            str_cmp(account_system.users[i].username, username) == 0) {
            return 0; // Username exists
        }
    }
    
    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!account_system.users[i].is_active) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) return 0;
    
    // Create user
    User* user = &account_system.users[slot];
    str_cpy(user->username, username);
    accounts_hash_password(password, user->password_hash);
    user->level = level;
    user->is_active = 1;
    user->created_time = rdtsc64();
    user->last_login = 0;
    user->login_count = 0;
    
    // Set home directory
    str_cpy(user->home_dir, "/home/");
    str_concat(user->home_dir, username);
    
    account_system.user_count++;
    return 1;
}

int accounts_login(const char* username, const char* password) {
    // Find user
    for (int i = 0; i < MAX_USERS; i++) {
        if (account_system.users[i].is_active &&
            str_cmp(account_system.users[i].username, username) == 0) {
            
            // Verify password
            if (accounts_verify_password(password, account_system.users[i].password_hash)) {
                account_system.current_user_id = i;
                account_system.is_logged_in = 1;
                account_system.users[i].last_login = rdtsc64();
                account_system.users[i].login_count++;
                return 1;
            }
            return 0; // Wrong password
        }
    }
    return 0; // User not found
}

void accounts_logout(void) {
    account_system.current_user_id = -1;
    account_system.is_logged_in = 0;
}

int accounts_delete_user(const char* username) {
    // Can't delete if not logged in as admin/root
    if (!accounts_has_permission(USER_LEVEL_ADMIN)) {
        return 0;
    }
    
    // Can't delete yourself
    if (account_system.is_logged_in) {
        const char* current = accounts_get_current_username();
        if (str_cmp(current, username) == 0) {
            return 0;
        }
    }
    
    // Find and delete user
    for (int i = 0; i < MAX_USERS; i++) {
        if (account_system.users[i].is_active &&
            str_cmp(account_system.users[i].username, username) == 0) {
            
            // Can't delete root
            if (account_system.users[i].level == USER_LEVEL_ROOT) {
                return 0;
            }
            
            account_system.users[i].is_active = 0;
            account_system.user_count--;
            return 1;
        }
    }
    return 0;
}

int accounts_change_password(const char* old_password, const char* new_password) {
    if (!account_system.is_logged_in) return 0;
    
    User* user = &account_system.users[account_system.current_user_id];
    
    // Verify old password
    if (!accounts_verify_password(old_password, user->password_hash)) {
        return 0;
    }
    
    // Set new password
    accounts_hash_password(new_password, user->password_hash);
    return 1;
}

int accounts_is_logged_in(void) {
    return account_system.is_logged_in;
}

const char* accounts_get_current_username(void) {
    if (!account_system.is_logged_in) {
        return "guest";
    }
    return account_system.users[account_system.current_user_id].username;
}

UserLevel accounts_get_current_level(void) {
    if (!account_system.is_logged_in) {
        return USER_LEVEL_GUEST;
    }
    return account_system.users[account_system.current_user_id].level;
}

int accounts_has_permission(UserLevel required_level) {
    if (!account_system.is_logged_in) {
        return required_level <= USER_LEVEL_GUEST;
    }
    
    UserLevel current_level = account_system.users[account_system.current_user_id].level;
    return current_level >= required_level;
}

const char* accounts_level_to_string(UserLevel level) {
    switch (level) {
        case USER_LEVEL_GUEST: return "Guest";
        case USER_LEVEL_USER: return "User";
        case USER_LEVEL_ADMIN: return "Admin";
        case USER_LEVEL_ROOT: return "Root";
        default: return "Unknown";
    }
}

int accounts_list_users(char output[][128], int max_lines) {
    int line = 0;
    
    for (int i = 0; i < MAX_USERS && line < max_lines; i++) {
        if (account_system.users[i].is_active) {
            User* user = &account_system.users[i];
            
            str_cpy(output[line], "  ");
            str_concat(output[line], user->username);
            
            // Pad to 20 chars
            int len = str_len(output[line]);
            while (len < 20) {
                str_concat(output[line], " ");
                len++;
            }
            
            str_concat(output[line], accounts_level_to_string(user->level));
            
            // Add login count
            len = str_len(output[line]);
            while (len < 35) {
                str_concat(output[line], " ");
                len++;
            }
            
            char count_str[16];
            int_to_str(user->login_count, count_str);
            str_concat(output[line], count_str);
            str_concat(output[line], " logins");
            
            line++;
        }
    }
    
    return line;
}