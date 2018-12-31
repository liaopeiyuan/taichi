#include <taichi/common/util.h>
#include <taichi/io/io.h>
#include <set>

#include "codegen.h"
#include "vectorizer.h"
#include "util.h"
#include "program.h"

TC_NAMESPACE_BEGIN

namespace Tlang {

class TikzGen : public Visitor {
 public:
  std::string graph;
  TikzGen() : Visitor(Visitor::Order::parent_first) {
  }

  std::string expr_name(Expr expr) {
    std::string members = "";
    if (!expr) {
      TC_ERROR("expr = 0");
    }
    if (expr->is_vectorized) {
      members = "[";
      bool first = true;
      for (auto m : expr->members) {
        if (!first)
          members += ", ";
        members += fmt::format("{}", m->id);
        first = false;
      }
      members += "]";
    }
    return fmt::format("\"({}){}{}\"", expr->id, members,
                       expr->node_type_name());
  }

  void link(Expr a, Expr b) {
    graph += fmt::format("{} -> {}; ", expr_name(a), expr_name(b));
  }

  void visit(Expr &expr) override {
    for (auto &ch : expr->ch) {
      link(expr, ch);
    }
  }
};

void visualize_IR(std::string fn, Expr &expr) {
  TikzGen gen;
  expr.accept(gen);
  auto cmd =
      fmt::format("python3 {}/projects/taichi_lang/make_graph.py {} '{}'",
                  get_repo_dir(), fn, gen.graph);
  trash(system(cmd.c_str()));
}


#if (0)
// https://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#warp-shuffle-functions
class GPUCodeGen : public CodeGenBase {
 public:
  int simd_width;
  int group_size;

 public:
  GPUCodeGen() : CodeGenBase() {
#if !defined(CUDA_FOUND)
    TC_ERROR("No GPU/CUDA support.");
#endif
    suffix = "cu";
    simd_width = 32;
  }

  std::string kernel_name() {
    return fmt::format("{}_kernel", func_name);
  }

  void codegen(Expr &vectorized_expr, int group_size = 1) {
    this->group_size = group_size;
    TC_ASSERT(group_size != 0);
    // group_size = expr->ch.size();
    num_groups = simd_width / group_size;
    TC_WARN_IF(simd_width % group_size != 0, "insufficient lane usage");

    emit_code(
        "#include <common.h>\n using namespace taichi; using namespace "
        "Tlang;\n\n");

    emit_code("__global__ void {}(Context context) {{", kernel_name());
    emit_code("auto n = context.get_range(0);\n");
    emit_code("auto linear_idx = blockIdx.x * blockDim.x + threadIdx.x;\n");
    emit_code("if (linear_idx >= n * {}) return;\n", group_size);
    emit_code("auto g = linear_idx / {} / ({} / {}); \n", group_size,
              simd_width, group_size);
    emit_code("auto sub_g_id = linear_idx / {} % ({} / {}); \n", group_size,
              simd_width, group_size);
    emit_code("auto g_idx = linear_idx % {}; \n", group_size);
    emit_code("int l = threadIdx.x & 0x1f;\n");
    emit_code("int loop_index = 0;");

    // Body
    TC_DEBUG("Vectorizing");
    vectorized_expr.accept(*this);
    TC_DEBUG("Vectorizing");

    emit_code("}\n\n");

    emit_code("extern \"C\" void " + func_name + "(Context context) {\n");
    emit_code("auto n = context.get_range(0);\n");
    int block_size = 256;
    emit_code("{}<<<n * {} / {}, {}>>>(context);", kernel_name(), group_size,
              block_size, block_size);
    emit_code("cudaDeviceSynchronize();\n");

    emit_code("}\n");
  }

  void visit(Expr &expr) override {
    /*
    TC_ASSERT(expr->is_vectorized);
    TC_ASSERT(expr->members.size() == 0 ||
              (int)expr->members.size() == group_size);
    if (expr->type == NodeType::addr) {
      return;
    }
    // TC_P(expr->ch.size());
    if (expr->var_name == "")
      expr->var_name = create_variable();
    else
      return;  // visited
    if (binary_ops.find(expr->type) != binary_ops.end()) {
      auto op = binary_ops[expr->type];
      emit_code("auto {} = {} {} {}; \\\n", expr->var_name,
                expr->ch[0]->var_name, op, expr->ch[1]->var_name);
    } else if (expr->type == NodeType::load) {
      auto buffer_name = fmt::format("buffer{:02d}", expr->addr().buffer_id);
      std::vector<int> offsets;
      for (int i = 0; i + 1 < (int)expr->members.size(); i++) {
        TC_ASSERT(
            expr->members[i]->addr().same_type(expr->members[i + 1]->addr()));
      }
      for (int i = 0; i < (int)expr->members.size(); i++) {
        offsets.push_back(expr->members[i]->addr().offset());
      }
      auto addr = expr->addr();
      auto i_stride = num_groups;
      TC_ASSERT(i_stride == addr.coeff_aosoa_group_size);
      if (addr.coeff_const % simd_width != 0) {
        addr.coeff_const -= addr.coeff_const % simd_width;
      }

      if (group_size > 1) {
        // detect patterns
        int offset_const = offsets[0] % simd_width;
        int offset_inc = offsets[1] - offsets[0];
        for (int i = 0; i + 1 < (int)offsets.size(); i++) {
          TC_ASSERT(offset_inc == offsets[i + 1] - offsets[i]);
        }
        emit_code("auto {} = {}; \\\n", expr->var_name,
                  get_vectorized_address(addr, offset_const, offset_inc));
      } else {
        emit_code("auto {} = {}; \\\n", expr->var_name,
                  get_vectorized_address(addr));
      }
    } else if (expr->type == NodeType::store) {
      emit_code("{} = {}; \\\n", get_vectorized_address(expr->addr()),
                expr->ch[1]->var_name);
    } else if (expr->type == NodeType::combine) {
      // do nothing
    } else {
      TC_P((int)expr->type);
      TC_NOT_IMPLEMENTED;
    }
    */
  }

  // group_size should be batch_size here...
  FunctionType compile() {
    write_code_to_file();
    auto cmd = fmt::format(
        "nvcc {} -std=c++14 -shared -O3 -Xcompiler \"-fPIC\" --use_fast_math "
        "--ptxas-options=-allow-expensive-optimizations=true,-O3 -I {}/headers "
        "-ccbin g++-6 "
        "-D_GLIBCXX_USE_CXX11_ABI=0 -DTLANG_GPU -o {} 2> {}.log",
        get_source_fn(), get_project_fn(), get_library_fn(), get_source_fn());
    auto compile_ret = std::system(cmd.c_str());
    TC_ASSERT(compile_ret == 0);
#if defined(TC_PLATFORM_LINUX)
    auto objdump_ret = system(
        fmt::format("objdump {} -d > {}.s", get_library_fn(), get_library_fn())
            .c_str());
    trash(objdump_ret);
#endif
    return load_function();
  }

  FunctionType get(Program &prog) {
    auto e = prog.ret;
    group_size = prog.config.group_size;
    simd_width = 32;
    TC_ASSERT(simd_width == 32);
    auto vectorized_expr = Vectorizer(simd_width).run(e, group_size);
    codegen(vectorized_expr, group_size);
    return compile();
  }

  // Create vectorized IR for the root node
  // the vector width should be the final SIMD instruction width
  std::string get_vectorized_address(Address addr,
                                     int extra_offset = 0,
                                     int g_idx_inc = 1) {
    TC_ASSERT(addr.buffer_id != -1);
    auto buffer_name =
        fmt::format("context.get_buffer<float32>({:02d})", addr.buffer_id);
    auto warp_stride =
        addr.coeff_i * num_groups +
        num_groups / addr.coeff_aosoa_group_size * addr.coeff_aosoa_stride;
    auto offset = addr.coeff_const;
    return fmt::format(
        "{}[{} * n + {} * (g + loop_index) + sub_g_id * {} + {} + {} + g_idx * "
        "{}]",
        buffer_name, addr.coeff_imax, warp_stride, addr.coeff_i, offset,
        extra_offset, (g_idx_inc));
  }

  /*
  std::string get_vectorized_index(Address addr,
                                   int extra_offset = 0,
                                   int g_idx_inc = 1) {
    TC_ASSERT(addr.buffer_id != -1);
    auto buffer_name =
        fmt::format("context.get_buffer<float32>({:02d})", addr.buffer_id);
    auto warp_stride =
        addr.coeff_i * num_groups +
        num_groups / addr.coeff_aosoa_group_size * addr.coeff_aosoa_stride;
    auto offset = addr.coeff_const;
    return fmt::format(
        "{} * n + {} * (g + loop_index) + sub_g_id * {} + {} + {} + g_idx * "
        "{}",
        addr.coeff_imax, warp_stride, addr.coeff_i, offset, extra_offset,
        g_idx_inc);
  }
  */
};
#endif

void CPUCodeGen::codegen(Program &prog, int group_size) {
  // TC_ASSERT(mode == Mode::vector);
  this->group_size = group_size;
  TC_ASSERT(group_size != 0);
  this->prog = &prog;
  // group_size = expr->ch.size();
  num_groups = prog.config.num_groups;
  TC_WARN_IF(simd_width % group_size != 0, "insufficient lane usage");

  generate_header();

  // emit_code("float32 {}[128];", get_cache_name(0));

  start_macro_loop();

  // Adapters
  for (int i = 0; i < (int)prog.adapters.size(); i++) {
    auto &ad = prog.adapters[i];
    create_adapter(ad.dt, i, ad.counter / ad.input_group_size,
                   ad.input_group_size, ad.output_group_size);
  }

  // Body

  // adapters
  for (auto adapter : prog.adapters) {
    auto old_gs = this->group_size;
    TC_P(adapter.stores->ch.size());
    auto vectorized_cache_stores =
        Vectorizer().run(adapter.stores, adapter.input_group_size);

    this->group_size = adapter.input_group_size;
    vectorized_cache_stores.accept(*this);

    this->group_size = old_gs;
  }

  // main
  {
    TC_ASSERT(prog.ret);
    // visualize_IR(get_source_fn() + ".scalar.pdf", prog.ret);
    this->group_size = group_size;
    // TC_P(group_size);
    auto vectorized_stores =
        Vectorizer().run(prog.ret, prog.config.group_size);
    // visualize_IR(get_source_fn() + ".vector.pdf", vectorized_stores);
    vectorized_stores.accept(*this);
  }
  end_macro_loop();

  code_suffix = "";
  generate_tail();
}

FunctionType CPUCodeGen::get(Program &prog) {
  auto group_size = prog.config.group_size;
  // auto mode = CPUCodeGen::Mode::vv;
  auto mode = CPUCodeGen::Mode::intrinsics;
  auto simd_width = 8;
  this->mode = mode;
  this->simd_width = simd_width;
  codegen(prog, group_size);
  return compile();
}

FunctionType Program::compile() {
  FunctionType ret;
  materialize_layout();
  if (config.simd_width == -1) {
    config.simd_width = default_simd_width(config.arch);
  }
  if (config.num_groups == -1) {
    config.num_groups = config.simd_width / config.group_size;
  }
  TC_ASSERT(config.group_size > 0);
  if (config.arch == Arch::x86_64) {
    CPUCodeGen codegen;
    codegen.unroll = 1;
    ret = codegen.get(*this);
  } else if (config.arch == Arch::gpu) {
    TC_NOT_IMPLEMENTED
    // GPUCodeGen codegen;
    // function = codegen.get(*this);
  } else {
    TC_NOT_IMPLEMENTED;
  }
  return ret;
}
}

TC_NAMESPACE_END
