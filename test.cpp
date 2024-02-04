

#include "mkn/kul/signal.hpp"
#include "mkn/kul/yaml.hpp"
#include <maiken.hpp>

std::string const yArgs = "";

int main(int argc, char *argv[]) {
  mkn::kul::Signal sig;
  try {
  } catch (mkn::kul::Exception const &e) {
    KLOG(ERR) << e.what();
    return 2;
  } catch (std::exception const &e) {
    KERR << e.what();
    return 3;
  } catch (...) {
    KERR << "UNKNOWN EXCEPTION TYPE CAUGHT";
    return 5;
  }
  return 0;
}
