// Passive opponent for the farm test (tools/farm_test.py): drafts like the starter bot, then never acts.
#include <cambc.h>
#include <iostream>
#include "draft.h"

int main() {
    while (true) {
        try {
            if (cambc::is_draft()) play_draft();
        } catch (const std::exception& error) {
            std::cout << "bot error: " << error.what() << std::endl;
        }
        cambc::end_turn();
    }
}
