/**
 * @file        codegen/codegen_writer.cpp
 * @brief       Consolidated codegen output writer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/codegen/codegen_writer.h>
#include "codegen_flags.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>
#include <inja/inja.hpp>

#include <rex/codegen/function_graph.h>
#include <rex/codegen/template_registry.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>

#include "codegen_logging.h"
#include "template_registry_internal.h"

namespace {

nlohmann::json buildTemplateData(const rex::codegen::CodegenContext& ctx,
                                 const std::vector<const rex::codegen::FunctionNode*>& functions,
                                 const std::unordered_map<uint32_t, std::string>& rexcrtByAddr) {
  const auto& cfg = ctx.Config();

  // Compute code_base and code_size from binary sections
  size_t codeMin = ~size_t(0);
  size_t codeMax = 0;
  for (const auto& section : ctx.binary().sections()) {
    if (section.executable) {
      if (section.baseAddress < codeMin)
        codeMin = section.baseAddress;
      if ((section.baseAddress + section.size) > codeMax)
        codeMax = section.baseAddress + section.size;
    }
  }

  // Build functions JSON array
  nlohmann::json functionsJson = nlohmann::json::array();
  for (const auto* fn : functions) {
    std::string funcName;
    bool isRexcrt = false;

    auto crtIt = rexcrtByAddr.find(static_cast<uint32_t>(fn->base()));
    if (crtIt != rexcrtByAddr.end()) {
      funcName = crtIt->second;
      isRexcrt = true;
    } else if (fn->base() == ctx.analysisState().entryPoint) {
      funcName = "xstart";
    } else if (!fn->name().empty()) {
      funcName = fn->name();
    } else {
      funcName = fmt::format("sub_{:08X}", fn->base());
    }

    functionsJson.push_back({
        {"address", fmt::format("0x{:X}", fn->base())},
        {"name", funcName},
        {"is_rexcrt", isRexcrt},
        {"below_code_base", (fn->base() < codeMin)},
        {"is_import", fn->authority() == rex::codegen::FunctionAuthority::IMPORT},
    });
  }

  // Build config flags
  nlohmann::json configFlags = {
      {"skip_lr", cfg.skipLr},
      {"ctr_as_local", cfg.ctrAsLocalVariable},
      {"xer_as_local", cfg.xerAsLocalVariable},
      {"reserved_as_local", cfg.reservedRegisterAsLocalVariable},
      {"skip_msr", cfg.skipMsr},
      {"cr_as_local", cfg.crRegistersAsLocalVariables},
      {"non_argument_as_local", cfg.nonArgumentRegistersAsLocalVariables},
      {"non_volatile_as_local", cfg.nonVolatileRegistersAsLocalVariables},
  };

  return {
      {"project", cfg.projectName},
      {"image_base", fmt::format("0x{:X}", ctx.binary().baseAddress())},
      {"image_size", fmt::format("0x{:X}", ctx.binary().imageSize())},
      {"code_base", fmt::format("0x{:X}", codeMin)},
      {"code_size", fmt::format("0x{:X}", codeMax - codeMin)},
      {"rexcrt_heap", cfg.rexcrtFunctions.contains("RtlAllocateHeap") ? 1 : 0},
      {"thunk_reserve_size", fmt::format("0x{:X}", 0x10000u)},
      {"has_dll_modules", ctx.hasDllModules()},
      {"is_dll", ctx.isDllModule()},
      {"config_flags", configFlags},
      {"functions", functionsJson},
      {"recomp_files", nlohmann::json::array()},
      {"include_all_function_declarations", true},
  };
}

}  // namespace

namespace rex::codegen {

constexpr size_t kOutputBufferReserveSize = 32 * 1024 * 1024;  // 32 MB

CodegenWriter::CodegenWriter(CodegenContext& ctx, Runtime* runtime)
    : ctx_(ctx), runtime_(runtime) {}

// Convenience accessors
FunctionGraph& CodegenWriter::graph() {
  return ctx_.graph;
}
const FunctionGraph& CodegenWriter::graph() const {
  return ctx_.graph;
}
const BinaryView& CodegenWriter::binary() const {
  return ctx_.binary();
}
RecompilerConfig& CodegenWriter::config() {
  return ctx_.Config();
}
const RecompilerConfig& CodegenWriter::config() const {
  return ctx_.Config();
}
AnalysisState& CodegenWriter::analysisState() {
  return ctx_.analysisState();
}
const AnalysisState& CodegenWriter::analysisState() const {
  return ctx_.analysisState();
}

bool CodegenWriter::write(bool force) {
  // --- Validation gate (from recompile.cpp) ---
  if (ctx_.errors.HasErrors() && !force) {
    REXCODEGEN_ERROR("Code generation blocked: {} validation errors. Use --force to override.",
                     ctx_.errors.Count());
    return false;
  }

  // --- Output directory setup (from recompile.cpp) ---
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;
  REXCODEGEN_TRACE("Output path: {}", outputPath.string());
  std::filesystem::create_directories(outputPath);

  // --- Clean old generated files (from recompile.cpp) ---
  std::string prefix = config().projectName + "_";
  for (const auto& entry : std::filesystem::directory_iterator(outputPath)) {
    auto ext = entry.path().extension();
    if (ext == ".cpp" || ext == ".h" || ext == ".cmake") {
      std::string filename = entry.path().filename().string();
      if (filename == "sources.cmake" || filename.starts_with(prefix) ||
          filename.starts_with("ppc_recomp") || filename.starts_with("ppc_func_mapping") ||
          filename.starts_with("function_table_init") || filename.starts_with("ppc_config")) {
        deletedFiles_.push_back(filename);
        std::filesystem::remove(entry.path());
      }
    }
  }

  // --- Everything below from recompiler.cpp recompile() ---
  REXCODEGEN_TRACE("Recompile: starting");
  out.reserve(kOutputBufferReserveSize);

  // Build sorted function list from graph
  std::vector<const FunctionNode*> functions;
  functions.reserve(graph().functionCount());
  for (const auto& [addr, node] : graph().functions()) {
    functions.push_back(node.get());
  }
  std::sort(functions.begin(), functions.end(),
            [](const auto* a, const auto* b) { return a->base() < b->base(); });

  // Build rexcrt reverse map and rename graph nodes
  std::unordered_map<uint32_t, std::string> rexcrtByAddr;
  for (const auto& [name, addr] : config().rexcrtFunctions) {
    auto crtName = fmt::format("rexcrt_{}", name);
    rexcrtByAddr[addr] = crtName;
    if (auto* node = graph().getFunction(addr)) {
      node->setName(std::move(crtName));
    }
  }

  const std::string& projectName = config().projectName;

  TemplateRegistry registry;
  if (!config().templateDir.empty())
    registry.loadOverrides(config().templateDir);

  auto tmplData = buildTemplateData(ctx_, functions, rexcrtByAddr);
  std::unordered_set<std::string> knownFunctionNames;
  knownFunctionNames.reserve(tmplData["functions"].size());
  for (const auto& functionData : tmplData["functions"])
    knownFunctionNames.insert(functionData["name"].get<std::string>());

  // Generate {project}_init.h (self-contained: config + declarations + macros)
  REXCODEGEN_TRACE("Recompile: generating {}_init.h", projectName);
  out = renderWithJson(registry, "codegen/init_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.h", projectName));

  // Generated recompilation units need stable macros and import declarations,
  // but a new function must not invalidate every unit through this header.
  REXCODEGEN_TRACE("Recompile: generating {}_recomp.h", projectName);
  tmplData["include_all_function_declarations"] = false;
  out = renderWithJson(registry, "codegen/init_h", tmplData);
  SaveCurrentOutData(fmt::format("{}_recomp.h", projectName));
  tmplData["include_all_function_declarations"] = true;

  // Generate {project}_init.cpp (PPCImageConfig + PPCFuncMappings)
  REXCODEGEN_TRACE("Recompile: generating {}_init.cpp", projectName);
  out = renderWithJson(registry, "codegen/init_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_init.cpp", projectName));

  // Generate {project}_register.cpp (registration function for hash-based dispatch)
  REXCODEGEN_TRACE("Recompile: generating {}_register.cpp", projectName);
  tmplData["is_dll"] = ctx_.isDllModule();
  out = renderWithJson(registry, "codegen/register_cpp", tmplData);
  SaveCurrentOutData(fmt::format("{}_register.cpp", projectName));

  // Filter out imports and rexcrt functions before recompilation
  std::erase_if(functions, [](const FunctionNode* fn) {
    return fn->authority() == FunctionAuthority::IMPORT;
  });
  std::erase_if(functions, [&rexcrtByAddr](const FunctionNode* fn) {
    return rexcrtByAddr.contains(static_cast<uint32_t>(fn->base()));
  });
  std::erase_if(functions, [&](const FunctionNode* fn) {
    auto configIt = config().functions.find(fn->base());
    if (configIt == config().functions.end() || !configIt->second.isChunk()) {
      return false;
    }
    const auto* parent = graph().getFunction(configIt->second.parent);
    return parent && parent->containsBlockAddress(fn->base());
  });

  // Build EmitContext -- resolver is now properly connected
  EmitContext emitCtx{binary(), config(), graph(),
                      static_cast<uint32_t>(analysisState().entryPoint), nullptr};
  if (runtime_)
    emitCtx.resolver = runtime_->export_resolver();

  // Generate recomp files in stable guest-address shards. Size-only splitting
  // moves every later function when one entry is inserted. Stable shards keep
  // that invalidation inside one bounded guest address span.
  REXCODEGEN_TRACE("Recompiling {} functions...", functions.size());
  struct PendingFunction {
    const FunctionNode* function;
    std::string code;
  };
  std::vector<PendingFunction> pendingFunctions;
  size_t currentFileBytes = 0;
  uint32_t currentShardBase = 0;
  size_t currentShardPart = 0;
  bool hasShard = false;

  auto emittedFunctionName = [&](const FunctionNode* function) {
    if (function->base() == emitCtx.entryPoint)
      return std::string("xstart");
    if (!function->name().empty())
      return function->name();
    return fmt::format("sub_{:08X}", function->base());
  };

  auto flushRecompUnit = [&]() {
    if (pendingFunctions.empty())
      return;
    std::set<std::string> declarations;
    auto collectTarget = [&](const CallTarget& target) {
      if (target.isFunction()) {
        const auto* targetFunction = target.asFunction();
        if (targetFunction)
          declarations.insert(emittedFunctionName(targetFunction));
      }
    };
    for (const auto& pending : pendingFunctions) {
      declarations.insert(emittedFunctionName(pending.function));
      for (const auto& edge : pending.function->calls())
        collectTarget(edge.target);
      for (const auto& edge : pending.function->tailCalls())
        collectTarget(edge.target);

      // Some direct calls are created during emission. Late jump tables and
      // exception handlers do not exist in the stored call-edge lists.
      std::string_view code = pending.code;
      constexpr std::string_view callSuffix = "(ctx, base)";
      size_t position = 0;
      while ((position = code.find(callSuffix, position)) != std::string_view::npos) {
        size_t start = position;
        while (start > 0) {
          const char value = code[start - 1];
          if (!(std::isalnum(static_cast<unsigned char>(value)) || value == '_'))
            break;
          --start;
        }
        if (start < position) {
          std::string name(code.substr(start, position - start));
          if (knownFunctionNames.contains(name))
            declarations.insert(std::move(name));
        }
        position += callSuffix.size();
      }
    }

    println("#include \"{}_recomp.h\"\n", projectName);
    for (const auto& declaration : declarations)
      println("DECLARE_REX_FUNC({});", declaration);
    if (!declarations.empty())
      println("");
    for (auto& pending : pendingFunctions) {
      out += pending.code;
      std::string().swap(pending.code);
    }

    auto filename = fmt::format("{}_recomp.{:08X}.{}.cpp", projectName, currentShardBase,
                                currentShardPart);
    recompFiles_.push_back(filename);
    SaveCurrentOutData(filename);
    FlushPendingWrites();
    pendingFunctions.clear();
    currentFileBytes = 0;
  };

  const uint32_t addressShardBytes = REXCVAR_GET(stable_address_shard_bytes);
  for (const auto* function : functions) {
    std::string code = function->emitCpp(emitCtx);
    const uint32_t shardBase =
        static_cast<uint32_t>((function->base() / addressShardBytes) * addressShardBytes);
    const bool newAddressShard = !hasShard || shardBase != currentShardBase;
    const bool sizeSplit = !pendingFunctions.empty() &&
                           currentFileBytes + code.size() > REXCVAR_GET(max_file_size_bytes);
    if (newAddressShard || sizeSplit) {
      flushRecompUnit();
      if (newAddressShard) {
        currentShardBase = shardBase;
        currentShardPart = 0;
        hasShard = true;
      } else {
        ++currentShardPart;
      }
    }

    if (code.size() > REXCVAR_GET(max_file_size_bytes)) {
      REXCODEGEN_WARN("Function 0x{:08X} is {} bytes, exceeds max_file_size_bytes ({})",
                      function->base(), code.size(), REXCVAR_GET(max_file_size_bytes));
    }
    currentFileBytes += code.size();
    pendingFunctions.push_back({function, std::move(code)});
  }
  flushRecompUnit();
  REXCODEGEN_TRACE("Recompilation complete.");

  // Generate sources.cmake
  REXCODEGEN_TRACE("Recompile: generating sources.cmake");
  {
    auto& recompFiles = tmplData["recomp_files"];
    recompFiles = nlohmann::json::array();
    for (const auto& filename : recompFiles_)
      recompFiles.push_back(filename);
    out = renderWithJson(registry, "codegen/sources_cmake", tmplData);
    SaveCurrentOutData("sources.cmake");
  }

  // Write all buffered files to disk
  FlushPendingWrites();
  return true;
}

void CodegenWriter::SaveCurrentOutData(const std::string_view name) {
  if (!out.empty()) {
    std::string filename;

    if (name.empty()) {
      filename = fmt::format("{}_recomp.{}.cpp", config().projectName, cppFileIndex);
      ++cppFileIndex;
    } else {
      filename = std::string(name);
    }

    pendingWrites.emplace_back(std::move(filename), std::move(out));
    out.clear();
  }
}

void CodegenWriter::FlushPendingWrites() {
  std::filesystem::path outputPath = ctx_.configDir() / config().outDirectoryPath;

  for (const auto& [filename, content] : pendingWrites) {
    std::string filePath = (outputPath / filename).string();
    REXCODEGEN_TRACE("flush_pending_writes: filePath={}", filePath);

    FILE* f = rex::filesystem::OpenFile(rex::to_path(filePath), "wb");
    if (!f) {
      REXCODEGEN_ERROR("Failed to open file for writing: {}", filePath);
      continue;
    }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    REXCODEGEN_TRACE("Wrote {} bytes to {}", content.size(), filePath);

    writtenFiles_.push_back(filename);
  }

  pendingWrites.clear();
}

}  // namespace rex::codegen
