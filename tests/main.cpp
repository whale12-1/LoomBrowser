#include <catch2/catch_session.hpp>
#include <vector>
#include <string>

int main(int argc, char* argv[]) {
    Catch::Session session;

    // Исключаем флаг -qmljsdebugger из списка аргументов
    std::vector<char*> filtered_args;
    for (int i = 0; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("-qmljsdebugger") == std::string::npos) {
            filtered_args.push_back(argv[i]);
        }
    }

    int returnCode = session.applyCommandLine(static_cast<int>(filtered_args.size()), filtered_args.data());
    if (returnCode != 0) {
        return returnCode;
    }

    return session.run();
}