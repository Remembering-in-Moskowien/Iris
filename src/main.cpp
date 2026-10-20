#include "iris/cli/cli.hpp"
#include "iris/infra/log.hpp"

int main(int argc, char** argv) {
    iris::infra::init_logger();
    return iris::cli::run(argc, argv);
}
