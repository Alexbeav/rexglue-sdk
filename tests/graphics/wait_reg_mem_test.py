"""Compile the production WAIT_REG_MEM handler with a bounded, headless backend.

The handler is extracted unchanged so this catches hook placement regressions
without creating a GPU device or depending on private game command streams.
"""
import argparse
from pathlib import Path
import subprocess


PREFIX = r'''
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>
#define SCOPE_profile_cpu_f(x)
#define REXCVAR_GET(x) false
constexpr uint32_t XE_GPU_REG_COHER_STATUS_HOST = 0x1000;
namespace xenos { enum class Endian { kNone }; uint32_t GpuSwap(uint32_t v, Endian) { return v; } }
namespace memory {
struct RingBuffer {
  std::vector<uint32_t> words; size_t index = 0;
  template<typename T> T ReadAndSwap() { return T(words.at(index++)); }
};
}
namespace rex::thread {
int yields = 0;
void MaybeYield() { if (++yields >= 8) throw std::runtime_error("bounded spin limit"); }
void Sleep(std::chrono::milliseconds) { MaybeYield(); }
void SyncMemory() {}
}
struct Memory {
  uint32_t value = 0; unsigned reads = 0;
  void* TranslatePhysical(uint32_t) { ++reads; return &value; }
};
struct Trace { void WriteMemoryRead(uint32_t, size_t) {} };
class CommandProcessor {
public:
  Memory memory; Memory* memory_ = &memory;
  Trace trace_writer_; bool worker_running_ = true;
  unsigned flushes = 0; uint32_t pending_value = 1; bool flush_ok = true;
  uint32_t CpuToGpu(uint32_t v) { return v; }
  uint32_t ReadRegisterValue(uint32_t) { return 1; }
  void MakeCoherent() {}
  void PrepareForWait() {}
  bool PrepareForGuestMemoryRead() {
    ++flushes;
    if (!flush_ok) return false;
    memory.value = pending_value;
    return true;
  }
  void ReturnFromWait() {}
  bool ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer*, uint32_t, uint32_t);
};
'''

SUFFIX = r'''
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
bool poll(CommandProcessor& cp, uint32_t wait, bool memory = true) {
  memory::RingBuffer packet{{memory ? 0x13u : 0x03u, 0u, 1u, 0xFFFFFFFFu, wait}};
  rex::thread::yields = 0;
  return cp.ExecutePacketType3_WAIT_REG_MEM(&packet, 0, 5);
}
int main() {
  try {
    for (uint32_t wait : {0u, 0xFFu, 0x100u}) {
      CommandProcessor cp;
      require(poll(cp, wait), "pending write did not complete");
      require(cp.flushes == 1 && cp.memory.reads == 1, "must flush before first read");
      require(rex::thread::yields == 0, "completed dependency should not spin");
    }
    {
      CommandProcessor cp;
      cp.memory.value = 1; cp.pending_value = 0;
      bool exhausted = false;
      try { require(!poll(cp, 0), "stale matching memory incorrectly passed"); }
      catch (const std::runtime_error& error) {
        exhausted = std::string(error.what()) == "bounded spin limit";
      }
      require(exhausted && cp.flushes == 1, "must poll fresh, nonmatching memory");
    }
    {
      CommandProcessor cp;
      cp.memory.value = 1; cp.flush_ok = false;
      require(!poll(cp, 0), "flush failure must stop packet execution");
      require(cp.flushes == 1 && cp.memory.reads == 0, "failed flush must not read stale data");
    }
    {
      CommandProcessor cp;
      require(poll(cp, 0, false), "register wait should still work");
      require(cp.flushes == 0 && cp.memory.reads == 0, "register wait must not flush guest memory");
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::cout << "6 WAIT_REG_MEM scenarios passed\n";
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = (root / "src/graphics/command_processor.cpp").read_text(encoding="utf-8")
    start = source.index("bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(")
    end = source.index("bool CommandProcessor::ExecutePacketType3_REG_RMW(", start)
    args.build_dir.mkdir(parents=True, exist_ok=True)
    cpp = args.build_dir / "wait_reg_mem_probe.cpp"
    exe = args.build_dir / "wait_reg_mem_probe.exe"
    cpp.write_text(PREFIX + source[start:end] + SUFFIX, encoding="utf-8")
    subprocess.run([args.compiler, "-std=c++20", str(cpp), "-o", str(exe)],
                   check=True, timeout=60)
    subprocess.run([str(exe.resolve())], check=True, timeout=10)


if __name__ == "__main__":
    main()
