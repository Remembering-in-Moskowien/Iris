#include "daisy/cli/cli.hpp"
#include "daisy/infra/log.hpp"

int main(int argc, char** argv) {
    daisy::infra::init_logger();
    return daisy::cli::run(argc, argv);
}
