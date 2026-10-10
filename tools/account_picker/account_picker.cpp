// PupBerg account picker
// finds the Steam accounts that logged in on this PC (config/loginusers.vdf)
// and writes the chosen one into the emu's global configs.user.ini

#include "account_picker_core.hpp"

int main()
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
#endif
    std::cout << "PupBerg account picker\n======================\n\n";
    int ret = pick_account_interactive();
    wait_for_enter();
    return ret;
}
