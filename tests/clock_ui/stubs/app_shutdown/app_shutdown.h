#pragma once
class AppShutdown {
public:
    inline static bool dialog = false;
    static bool isDialogOpen() { return dialog; }
};
