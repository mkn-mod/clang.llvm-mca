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

#include "mkn/kul/env.hpp"
#include "mkn/kul/io.hpp"
#include "mkn/kul/log.hpp"
#include "mkn/kul/os.hpp"
#include "mkn/kul/proc.hpp"
#include "mkn/kul/threads.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mkn::clang {

// Splits llvm-mca input into one assembly listing per LLVM-MCA-BEGIN marker. llvm-mca's own
// marker handling reads the text in order, which breaks on optimised loops: tail duplication
// copies a BEGIN into several predecessors, and loop rotation places the latch (END) above the
// header (BEGIN). Regions are instead built from the loop structure clang annotates the blocks
// with ("=>This Loop Header", "in Loop: Header=BBx_y", "Child Loop BBx_y").
class MCARegions {
  struct Block {
    std::string label, head;
    std::vector<std::string> body;
  };

  static bool is_block_start(std::string const& line) {
    static std::regex const label{R"(^(\.LBB\d+_\d+|[A-Za-z_][\w.$]*):)"};
    return line.starts_with("# %bb.") || std::regex_search(line, label);
  }

  static std::string label_of(std::string const& line) {
    std::string const pre = line.substr(0, line.find(':'));
    auto const end = pre.find_last_not_of(" \t");
    auto const start = pre.find_last_of(" \t", end);
    return pre.substr(start == std::string::npos ? 0 : start + 1, end - start);
  }

  static std::string loop_of(Block const& block) {
    static std::regex const header{R"(Header=(BB\d+_\d+))"};
    std::smatch m;
    if (std::regex_search(block.head, m, header)) return m[1];
    if (block.head.find("Loop Header") != std::string::npos) {
      auto const start = block.label.find_first_not_of(".L");
      return start == std::string::npos ? "" : block.label.substr(start);
    }
    return "";
  }

  // clang continues a label's loop annotation on the following comment-only lines, e.g. an inner
  // loop header is ".LBB2_7: # Parent Loop BB2_2" then "# =>  This Inner Loop Header", and an
  // outer header lists its "Child Loop BBx_y" lines
  static bool is_loop_note(std::string const& line) {
    auto const first = line.find_first_not_of(" \t");
    return first != std::string::npos && line[first] == '#' &&
           line.find("Loop") != std::string::npos;
  }

  static bool has_func_end(Block const& block) {
    for (auto const& line : block.body)
      if (line.starts_with(".Lfunc_end")) return true;
    return false;
  }

  // drop directives llvm-mca doesn't need, comments (including the markers) and debug labels
  static std::vector<std::string> code(std::vector<Block const*> const& sel) {
    static std::regex const skip{R"(^\s*(\.(loc|cfi_|p2align|file)|#|\.Ltmp\d+:))"};
    std::vector<std::string> out;
    for (auto const* block : sel)
      for (auto const& line : block->body)
        if (line.find_first_not_of(" \t") != std::string::npos && !std::regex_search(line, skip))
          out.emplace_back(line);
    return out;
  }

 public:
  // region name -> assembly lines, child loops are added as "<name>.<BBx_y>"
  using Regions = std::map<std::string, std::vector<std::string>>;

  static std::vector<std::string> read_lines(std::string const& path) {
    std::ifstream file{path, std::ios::binary};  // bytes, -g3 output can hold non-UTF-8 strings
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.emplace_back(std::move(line));
    }
    return lines;
  }

  static Regions extract(std::vector<std::string> const& lines) {
    static std::regex const child{R"(Child Loop (BB\d+_\d+))"};
    // comment-only lines, the marker text can also appear in string literals (.asciz)
    static std::regex const mark{R"(^\s*#\s*LLVM-MCA-(BEGIN|END)\s+(\S+))"};

    std::vector<Block> blocks;
    for (auto const& line : lines)
      if (is_block_start(line))
        blocks.emplace_back(Block{label_of(line), line, {}});
      else if (!blocks.empty() && blocks.back().body.empty() && is_loop_note(line))
        blocks.back().head += "\n" + line;
      else if (!blocks.empty())
        blocks.back().body.emplace_back(line);

    std::vector<std::string> loops;
    std::map<std::string, std::vector<Block const*>> members;
    std::map<std::string, std::vector<std::string>> kids;
    for (auto const& block : blocks) {
      auto const& loop = loops.emplace_back(loop_of(block));
      if (!loop.empty()) members[loop].emplace_back(&block);
      if (block.head.find("Loop Header") != std::string::npos) {
        auto& k = kids[loop];
        for (std::sregex_iterator it{block.head.begin(), block.head.end(), child}, end; it != end;
             ++it)
          k.emplace_back((*it)[1]);
      }
    }

    // a loop's own blocks followed by those of its child loops
    auto const whole_loop = [&](std::string const& loop) {
      std::vector<Block const*> sel = members[loop];
      for (auto const& c : kids[loop]) sel.insert(sel.end(), members[c].begin(), members[c].end());
      return sel;
    };

    Regions regions;
    for (std::size_t i = 0; i < blocks.size(); ++i)
      for (auto const& line : blocks[i].body) {
        std::smatch m;
        if (!std::regex_search(line, m, mark) || m[1] != "BEGIN") continue;
        std::string const name = m[2];
        if (regions.count(name)) continue;

        std::vector<Block const*> sel;
        if (auto const& loop = loops[i]; !loop.empty()) {
          for (auto const& c : kids[loop]) regions[name + "." + c] = code(members[c]);
          sel = whole_loop(loop);
        } else {
          // marker around a loop: up to the end of the function, not the END marker, as the
          // vectorised body can be placed after the END block
          for (std::size_t j = i; j < blocks.size(); ++j) {
            sel.emplace_back(&blocks[j]);
            if (has_func_end(blocks[j])) break;
          }
          for (auto const* block : sel)
            if (block->head.find("Loop Header") != std::string::npos) {
              auto const lp = loop_of(*block);
              regions[name + "." + lp] = code(whole_loop(lp));
            }
        }
        regions[name] = code(sel);
      }
    return regions;
  }
};

class LLVM_MCA_Module : public mkn::mod::Module {
  static constexpr std::string_view ALLOW_FAILURE_ENV = "MKN_LLVM_MCA_ALLOW_FAILURE";
  static constexpr std::string_view DEFAULT_STRIP =
      "-pg:-g*:-fsave-optimization-record*:-foptimization-record-*";
  static constexpr std::string_view HEADER =
      "#ifndef MKN_MCA_HPP\n"
      "#define MKN_MCA_HPP\n"
      "#if defined(__GNUC__) || defined(__clang__)\n"
      "#define MKN_MCA_BEGIN(n) __asm volatile(\"# LLVM-MCA-BEGIN \" #n)\n"
      "#define MKN_MCA_END(n) __asm volatile(\"# LLVM-MCA-END \" #n)\n"
      "#else\n"
      "#define MKN_MCA_BEGIN(n)\n"
      "#define MKN_MCA_END(n)\n"
      "#endif\n"
      "#endif\n";

  struct Job {
    std::string compiler, in, name;
    std::vector<std::string> args;
    mkn::kul::File asmFile;
    mkn::kul::Dir tmpDir, resDir;  // per TU: <region>.s / <region>.llvm-mca.txt
  };

  struct Shared {
    std::vector<std::string> mcaArgs;
    bool regionsOnly = false;
    std::mutex mutex;  // guards failures and output
    std::vector<std::string> failures;

    void fail(std::string const& msg) {
      std::lock_guard<std::mutex> lock(mutex);
      failures.emplace_back(msg);
    }
  };

  static std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
  }

  static std::string drop_file_type(std::string const& filename) {
    return filename.substr(0, filename.rfind("."));
  }

  // first token of a compile command that isn't a launcher, e.g. "ccache mpicxx" -> mpicxx
  static std::string real_compiler(std::string const& command) {
    static std::unordered_set<std::string> const launchers{"ccache", "sccache", "distcc", "icecc"};
    for (auto const& token : mkn::kul::cli::asArgs(command))
      if (!launchers.count(mkn::kul::File(token).name())) return token;
    return "";
  }

  // clang ships llvm-mca next to itself, e.g. clang++-19 -> /usr/lib/llvm-19/bin/llvm-mca, so
  // analysis matches the compiler's LLVM version. MPI wrappers pass the flag through to the
  // underlying compiler (OMPI_CXX etc), non-clang compilers echo the name back.
  static std::string guess_mca_bin(std::string const& compiler) {
    try {
      mkn::kul::Process p{compiler};
      mkn::kul::ProcessCapture pc{p};
      p << "-print-prog-name=llvm-mca";
      p.start();
      std::string const guess = trim(pc.outs());
      if (guess.find('/') != std::string::npos && mkn::kul::File(guess)) return guess;
    } catch (...) {
    }
    return "llvm-mca";
  }

  // -march=/-mcpu= from the compile flags, last one wins as with the compiler.
  static std::string mcpu_from(std::vector<std::string> const& args) {
    std::string cpu;
    for (auto const& a : args)
      for (std::string_view const pre : {"-march=", "-mcpu="})
        if (a.starts_with(pre)) cpu = a.substr(pre.size());
    return cpu;
  }

  static bool stripped(std::string const& arg, std::vector<std::string> const& strip) {
    for (auto const& s : strip) {
      if (s.empty()) continue;
      if (s.back() == '*') {
        if (arg.starts_with(std::string_view{s}.substr(0, s.size() - 1))) return true;
      } else if (arg == s)
        return true;
    }
    return false;
  }

  static std::string inc_dir(std::string const& buildDir) {
    return mkn::kul::Dir::JOIN(mkn::kul::Dir::JOIN(buildDir, "llvm-mca"), "inc");
  }

  // runs p to completion, failures rethrown with the captured stderr
  static std::string exec(mkn::kul::Process& p, std::string const& what) {
    mkn::kul::ProcessCapture pc{p};
    KLOG(DBG) << p;
    try {
      p.start();
    } catch (mkn::kul::proc::ExitException const& e) {
      KEXCEPT(mkn::kul::Exception, what + " failed (exit " + std::to_string(e.code()) + ")\n" +
                                       p.toString() + "\n" + pc.errs());
    }
    return pc.outs();
  }

  // output is only written on success, a failed region leaves no file behind
  static void analyse(Shared& shared, Job const& job, std::string const& region,
                      mkn::kul::File const& asmFile) {
    try {
      mkn::kul::Process p{shared.mcaArgs[0]};
      for (std::size_t i = 1; i < shared.mcaArgs.size(); ++i) p << shared.mcaArgs[i];
      p << asmFile.mini();
      std::string const out = exec(p, "analysis of " + job.in + " region " + region);
      mkn::kul::io::Writer{mkn::kul::File{region + ".llvm-mca.txt", job.resDir}} << out;
    } catch (mkn::kul::Exception const& e) {
      shared.fail(e.what());
    }
  }

  static void run(Shared& shared, Job const& job) {
    job.resDir.rm();  // drop results of regions that no longer exist
    job.tmpDir.rm();
    job.resDir.mk();
    job.tmpDir.mk();

    try {
      mkn::kul::Process cc{job.compiler};
      for (auto const& a : job.args) cc << a;
      cc << "-S" << "-o" << job.asmFile.mini() << job.in;
      exec(cc, "-S compile of " + job.in);
    } catch (mkn::kul::Exception const& e) {
      return shared.fail(e.what());
    }

    auto const regions = MCARegions::extract(MCARegions::read_lines(job.asmFile.real()));
    if (regions.empty()) {
      {
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.regionsOnly) {
          KOUT(NON) << "llvm-mca: skipping " << job.in << ", no LLVM-MCA-BEGIN markers";
          return;
        }
        KOUT(NON) << "llvm-mca: warning " << job.in
                  << " has no LLVM-MCA-BEGIN markers, analysing whole TU";
      }
      return analyse(shared, job, job.name, job.asmFile);
    }

    for (auto const& [region, lines] : regions) {
      mkn::kul::File const asmFile{region + ".s", job.tmpDir};
      {
        mkn::kul::io::Writer w{asmFile};
        for (auto const& line : lines) w << line << "\n";
      }
      analyse(shared, job, region, asmFile);
    }
  }

  static bool allow_failure(YAML::Node const& node) {
    if (node["allow_failure"]) return mkn::kul::String::BOOL(node["allow_failure"].Scalar());
    if (auto const env = mkn::kul::env::GET(std::string{ALLOW_FAILURE_ENV}.c_str()); !env.empty())
      return mkn::kul::String::BOOL(env);
    return false;
  }

 public:
  // <buildDir>/llvm-mca/inc provides mkn/mca.hpp with MKN_MCA_BEGIN/END(name). The include is
  // added here, the header is written in compile() as "clean" runs between the two.
  void init(mkn::mod::Context& ctx, YAML::Node const& /*node*/) KTHROW(std::exception) override {
    if (auto const buildDir = ctx.state().get("buildDir"))
      ctx.compilerState().add(mkn::mod::IncludeInput{inc_dir(*buildDir), false});
  }

  void compile(mkn::mod::Context& ctx, YAML::Node const& /*node*/) KTHROW(std::exception) override {
    auto const buildDir = ctx.state().get("buildDir");
    if (!buildDir) return;
    mkn::kul::Dir const incMkn{mkn::kul::Dir::JOIN(inc_dir(*buildDir), "mkn")};
    incMkn.mk();
    mkn::kul::File const header{"mca.hpp", incMkn};
    if (!header) mkn::kul::io::Writer{header} << HEADER;
  }

  void link(mkn::mod::Context& ctx, YAML::Node const& node) KTHROW(std::exception) override {
    auto const state = ctx.state();
    std::string const buildDir = state.get("buildDir", ".");

    std::unordered_set<std::string> types;
    if (node["types"])
      for (auto const& s : mkn::kul::String::SPLIT(node["types"].Scalar(), ":")) types.insert(s);

    std::vector<std::string> strip;
    for (auto const& s : mkn::kul::String::SPLIT(
             node["strip"] ? node["strip"].Scalar() : std::string{DEFAULT_STRIP}, ":"))
      strip.emplace_back(s);

    std::size_t threads = 1;
    if (node["threads"])
      threads = mkn::kul::String::UINT16(node["threads"].Scalar());
    else if (auto const t = state.get("threads"))
      threads = mkn::kul::String::UINT16(*t);
    if (threads == 0) threads = 1;

    bool const allowFailure = allow_failure(node);

    mkn::kul::Dir const res{"res", buildDir};
    res.mk();
    mkn::kul::Dir const tmp{"tmp", buildDir};
    tmp.mk();

    std::vector<Job> jobs;
    std::string compiler;
    ctx.per_compiler_command([&](mkn::mod::CompileCommand const& cmd) {
      std::string const name = mkn::kul::File(cmd.in).name();
      auto const dot = name.rfind(".");
      if (!types.empty() && (dot == std::string::npos || !types.count(name.substr(dot + 1))))
        return;

      std::string const full = ctx.compileCommandFor(cmd.in);
      auto const firstSpace = full.find(' ');
      std::string const flags = full.substr(firstSpace + 1, full.rfind(" -o") - (firstSpace + 1));
      if (compiler.empty()) compiler = real_compiler(full);

      std::vector<std::string> args;
      for (std::string const& a : mkn::kul::cli::asArgs(flags))
        if (!stripped(a, strip)) args.emplace_back(a);

      // object name is "<hash of source path>-<file>.<obj ext>", unique per TU
      std::string const base = drop_file_type(mkn::kul::File(cmd.out).name());
      jobs.emplace_back(Job{full.substr(0, firstSpace), cmd.in, name, std::move(args),
                            mkn::kul::File{base + ".s", tmp},
                            mkn::kul::Dir{mkn::kul::Dir::JOIN(tmp.real(), base)},
                            mkn::kul::Dir{mkn::kul::Dir::JOIN(res.real(), base)}});
    });
    if (jobs.empty()) return;

    Shared shared;
    shared.regionsOnly = node["regions"] && node["regions"].Scalar() == "only";
    shared.mcaArgs.emplace_back(node["bin"] ? node["bin"].Scalar() : guess_mca_bin(compiler));
    std::string const mcpu = node["mcpu"] ? node["mcpu"].Scalar() : mcpu_from(jobs.front().args);
    shared.mcaArgs.emplace_back("-mcpu=" + (mcpu.empty() ? std::string{"native"} : mcpu));
    if (node["args"])
      for (auto const& a : mkn::kul::cli::asArgs(node["args"].Scalar()))
        shared.mcaArgs.emplace_back(a);
    KOUT(NON) << "llvm-mca: " << shared.mcaArgs[0] << " " << shared.mcaArgs[1];

    {
      mkn::kul::ThreadPool<> pool(threads);
      for (auto const& job : jobs) pool.async([&, &job = job]() { run(shared, job); });
      pool.finish().join();
      pool.rethrow();
    }

    if (shared.failures.empty()) return;
    std::string msg = "llvm-mca: " + std::to_string(shared.failures.size()) + " failure(s)";
    for (auto const& f : shared.failures) msg += "\n" + f;
    if (!allowFailure)
      KEXCEPT(mkn::kul::Exception, msg + "\nset \"allow_failure: true\" or " +
                                       std::string{ALLOW_FAILURE_ENV} + "=1 to continue");
    KOUT(NON) << msg;
  }
};

}  // namespace mkn::clang

extern "C" MKN_KUL_PUBLISH mkn::mod::Module* maiken_module_construct() {
  return new mkn ::clang ::LLVM_MCA_Module;
}

extern "C" MKN_KUL_PUBLISH void maiken_module_destruct(mkn::mod::Module* p) { delete p; }
