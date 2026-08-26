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
#include "mkn/mod/init.hpp"  // IWYU pragma: keep

#include "mkn/kul/io.hpp"
#include "mkn/kul/log.hpp"
#include "mkn/kul/os.hpp"
#include "mkn/kul/proc.hpp"

#include <unordered_set>

namespace mkn::clang {

class LLVM_MCA_Module : public mkn::mod::Module {
  static std::string drop_file_type(std::string filename) {
    filename = mkn::kul::File{filename}.name();
    return filename.substr(0, filename.rfind("."));
  }

 public:
  void link(mkn::mod::Context& ctx, YAML::Node const& node) KTHROW(std::exception) override {
    std::string const mca_bin = node["bin"] ? node["bin"].Scalar() : "llvm-mca";
    std::string const buildDir = ctx.state().get("buildDir", ".");

    std::unordered_set<std::string> types;
    if (node["types"])
      for (auto const& s : mkn::kul::String::SPLIT(node["types"].Scalar(), ":")) types.insert(s);

    mkn::kul::Dir const res{"res", buildDir};
    res.mk();
    mkn::kul::Dir const tmp{"tmp", buildDir};
    tmp.mk();

    KLOG(INF);
    ctx.per_compiler_command([&](mkn::mod::CompileCommand const& cmd) {
      std::string const name = mkn::kul::File(cmd.in).name();
      auto const dot = name.rfind(".");
      if (!types.empty() && (dot == std::string::npos || !types.count(name.substr(dot + 1))))
        return;

      std::string const full = ctx.compileCommandFor(cmd.in);
      auto const firstSpace = full.find(' ');
      std::string const compiler = full.substr(0, firstSpace);
      std::string const flags = full.substr(firstSpace + 1, full.rfind(" -o") - (firstSpace + 1));

      mkn::kul::File const asmFile{drop_file_type(cmd.in) + ".s", tmp};

      mkn::kul::Process cc{compiler};
      for (std::string const& a : mkn::kul::cli::asArgs(flags)) cc << a;
      cc << "-S" << "-o" << asmFile.mini() << cmd.in;
      KLOG(DBG) << cc;
      cc.start();

      mkn::kul::Process p{mca_bin};
      mkn::kul::ProcessCapture pc{p};
      p << asmFile.mini();
      KLOG(DBG) << p;
      p.start();

      mkn::kul::File const outfile{drop_file_type(cmd.in) + ".llvm-mca.txt", res};
      mkn::kul::io::Writer{outfile} << pc.outs();
    });
  }
};

}  // namespace mkn::clang

extern "C" MKN_KUL_PUBLISH mkn::mod::Module* maiken_module_construct() {
  return new mkn ::clang ::LLVM_MCA_Module;
}

extern "C" MKN_KUL_PUBLISH void maiken_module_destruct(mkn::mod::Module* p) { delete p; }
