#pragma once

#include "../MenuLoad/Includes.h"


class UserMenu : public Singleton<UserMenu> 
{
public:

    void RenderMenu();


private:
    friend class Singleton<UserMenu>;
    UserMenu() {};
    ~UserMenu() {};
};

