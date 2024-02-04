/**
Copyright (c) 2024, Philip Deegan.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

    * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
    * Neither the name of Philip Deegan nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <string_view>
#include <unordered_set>

#include "maiken/module/init.hpp"

namespace mkn::clang {

class AppHack : public maiken::Application {
  std::string_view constexpr static base0 = " -save-temps=obj";

  auto static drop_file_type(std::string filename) {
    mkn::kul::File file{filename};
    filename = file.name();
    return filename.substr(0, filename.rfind("."));
  }

public:
  auto update(maiken::Source const &s) {
    std::string arg = s.args() + std::string{base0};
    return maiken::Source{s.in(), arg};
  }
  void hack() {
    if (this->main_)
      this->main_ = update(*this->main_);
  }

  auto hack_link() { return drop_file_type(this->main_->in()) + ".s"; }

  bool valid() { return bool{this->main_}; }
};

class LLVM_MCA_Module : public maiken::Module {
public:
  void compile(maiken::Application &a, YAML::Node const &node)
      KTHROW(std::exception) override {
    reinterpret_cast<AppHack *>(&a)->hack();
  }

  void link(maiken::Application &a, YAML::Node const &node)
      KTHROW(std::exception) override {
    AppHack *hack = reinterpret_cast<AppHack *>(&a);
    if (!hack->valid())
      return;

    mkn::kul::Dir res{"res", a.buildDir()};
    res.mk();

    mkn::kul::Dir tmp{"tmp", a.buildDir()};
    mkn::kul::File sss{hack->hack_link(), tmp};

    mkn::kul::Process p{"llvm-mca-14"};
    mkn::kul::ProcessCapture pc{p};
    p << sss.mini();
    p.start();

    mkn::kul::File outfile{"llvm-mca.txt", res};
    mkn::kul::io::Writer{outfile} << pc.outs();
  }
};

} // namespace mkn::clang

extern "C" KUL_PUBLISH maiken::Module *maiken_module_construct() {
  return new mkn ::clang ::LLVM_MCA_Module;
}

extern "C" KUL_PUBLISH void maiken_module_destruct(maiken::Module *p) {
  delete p;
}
