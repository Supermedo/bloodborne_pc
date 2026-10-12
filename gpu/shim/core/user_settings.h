// bbport co-op: shadPS4's UserSettings singleton, holding only the user manager.
#pragma once
#include "core/user_manager.h"

class UserSettingsImpl {
public:
    static UserSettingsImpl* GetInstance() {
        static UserSettingsImpl instance;
        return &instance;
    }
    UserManager& GetUserManager() { return manager; }

private:
    UserManager manager;
};
#define UserSettings (*UserSettingsImpl::GetInstance())
#define UserManagement UserSettings.GetUserManager()
