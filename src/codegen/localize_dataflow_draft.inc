// DRAFT, not built and not verified (3 Nov 2026, docs/CODEGEN_SPEEDUP.md "What next"): a dataflow version of
// LocalizeRegisters (function_graph.cpp) that writes back only registers changed since the last sync and reloads
// only registers needed later. Kept for a later attempt; it would replace LocalizeRegisters and must pass the
// local_registers_verify lockstep runs before any use.

// Minimal sync (stage 2): per-statement dataflow over the function's own control flow decides what
// each sync point writes back and reloads, with the context still exact wherever anything outside can
// look at it:
//   - write back (before a call / indirect call / trap / MMIO access / return) only the registers
//     assigned since the last sync on some path into it (forward "dirty" analysis): every other
//     register's local still equals its context field;
//   - reload (after a call that returns here) only the registers that may be read before being
//     overwritten later (backward liveness), so a stale local is never read;
//   - load at entry only the registers read before written (the rest start zeroed; never read).
// A statement the pass cannot classify counts every register it names as both written and read.

using RegSet = std::bitset<256>;

struct LocalStmt {
  enum Kind : uint8_t { Plain, Label, Goto, CondGoto, Return, CondReturn, Call, TailCall, Trap, Mmio, IfOpen,
                        Switch, Close };
  Kind kind = Plain;
  std::string_view text;   // the line
  std::string target;      // goto label
  RegSet use, kill, dirty;  // read, fully overwritten without being read, possibly written
  std::vector<size_t> succ;
};

// Registers named in a piece of text, by the function's register index.
static void NamedRegisters(std::string_view t, const std::unordered_map<std::string, size_t>& index, RegSet& out) {
  auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  for (size_t i = 0; i < t.size();) {
    if (!isIdent(t[i]) || (i > 0 && (isIdent(t[i - 1]) || t[i - 1] == '.' || t[i - 1] == ':'))) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j < t.size() && isIdent(t[j]))
      ++j;
    if (auto it = index.find(std::string(t.substr(i, j - i))); it != index.end())
      out.set(it->second);
    i = j;
  }
}

static LocalizeSkip LocalizeRegisters(std::string_view body, LocalizeResult& res) {
  // Reservations, atomics, MMIO and the global lock: left as generated (synchronisation code the
  // lockstep verify cannot replay; none of it is hot).
  for (std::string_view k : {"compare_exchange", "REX_MM_", "GLOBAL_LOCK", "*(uint32_t*)REX_RAW_ADDR",
                             "*(uint64_t*)REX_RAW_ADDR"}) {
    if (body.find(k) != std::string_view::npos)
      return LocalizeSkip::Atomics;
  }
  // 1. Rename ctx.<register> to <register> and collect the set.
  std::set<std::string> used;
  std::string renamed;
  renamed.reserve(body.size());
  for (size_t i = 0; i < body.size();) {
    if (body.compare(i, 4, "ctx.") == 0 &&
        (i == 0 || !(std::isalnum(static_cast<unsigned char>(body[i - 1])) || body[i - 1] == '_'))) {
      size_t len = ParseLocalField(body, i + 4);
      if (len) {
        std::string name(body.substr(i + 4, len));
        used.insert(name);
        renamed += name;
        i += 4 + len;
        continue;
      }
    }
    renamed += body[i++];
  }
  if (used.empty())
    return LocalizeSkip::NoRegisters;

  // Context order (r, ctr / xer / reserved, cr, f, v).
  std::vector<std::string> order;
  auto addIf = [&](const std::string& n) {
    if (used.contains(n))
      order.push_back(n);
  };
  for (int i = 0; i < 32; ++i)
    addIf(fmt::format("r{}", i));
  addIf("ctr");
  addIf("xer");
  addIf("reserved");
  for (int i = 0; i < 8; ++i)
    addIf(fmt::format("cr{}", i));
  for (int i = 0; i < 32; ++i)
    addIf(fmt::format("f{}", i));
  for (int i = 0; i < 128; ++i)
    addIf(fmt::format("v{}", i));
  if (order.size() != used.size() || order.size() > 256)
    return LocalizeSkip::Unrecognised;  // a name outside the register file (e.g. r32)
  std::unordered_map<std::string, size_t> index;
  for (size_t i = 0; i < order.size(); ++i)
    index[order[i]] = i;

  // 2. Statements.
  std::vector<LocalStmt> st;
  for (size_t p = 0; p < renamed.size();) {
    size_t e = renamed.find('\n', p);
    if (e == std::string::npos)
      e = renamed.size();
    LocalStmt s;
    s.text = std::string_view(renamed).substr(p, e - p);
    st.push_back(s);
    p = e + 1;
  }
  const bool trailingNewline = !renamed.empty() && renamed.back() == '\n';

  auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  auto hasWholeCtx = [&](std::string_view l) {
    for (size_t q = l.find("ctx"); q != std::string_view::npos; q = l.find("ctx", q + 3)) {
      bool before = q == 0 || !isIdent(l[q - 1]);
      bool after = q + 3 >= l.size() || (!isIdent(l[q + 3]) && l[q + 3] != '.');
      if (before && after)
        return true;
    }
    return false;
  };
  auto isDirectCall = [&](std::string_view t) {
    static constexpr std::string_view kArgs = "(ctx, base);";
    if (t.size() <= kArgs.size() || !t.ends_with(kArgs))
      return false;
    auto name = t.substr(0, t.size() - kArgs.size());
    if (std::isdigit(static_cast<unsigned char>(name[0])))
      return false;
    for (char c : name)
      if (!isIdent(c))
        return false;
    return true;
  };
  static constexpr std::string_view kFull[] = {".u64 = ", ".s64 = ", ".f64 = "};

  std::unordered_map<std::string, size_t> labels;
  std::vector<size_t> openStack;  // indices of lines that opened a block
  std::vector<std::pair<size_t, size_t>> blocks;  // (open, close)
  for (size_t i = 0; i < st.size(); ++i) {
    auto& s = st[i];
    std::string_view l = s.text;
    size_t ind = 0;
    while (ind < l.size() && l[ind] == '\t')
      ++ind;
    std::string_view t = l.substr(ind);
    if (t.empty() || t.starts_with("//")) {
      s.kind = LocalStmt::Plain;
      continue;
    }
    if (ind == 0 && t.starts_with("loc_") && t.ends_with(":")) {
      s.kind = LocalStmt::Label;
      labels[std::string(t.substr(0, t.size() - 1))] = i;
      continue;
    }
    if (t == "}") {
      s.kind = LocalStmt::Close;
      if (!openStack.empty()) {
        blocks.push_back({openStack.back(), i});
        openStack.pop_back();
      }
      continue;
    }
    if (t.find("REX_MM_") != std::string_view::npos)
      return LocalizeSkip::Atomics;
    if (isDirectCall(t) || (t.starts_with("REX_CALL_INDIRECT_FUNC(") && t.ends_with(");"))) {
      bool tail = i + 1 < st.size() && st[i + 1].text == std::string(l.substr(0, ind)) + "return;";
      s.kind = tail ? LocalStmt::TailCall : LocalStmt::Call;
      NamedRegisters(t, index, s.use);  // an indirect call's target register
      s.kill.set();                     // after it, every live register is reloaded
      continue;
    }
    if ((t.starts_with("ppc_trap(ctx, base, ") && t.ends_with(");")) ||
        (t.starts_with("if (") && t.find(") ppc_trap(ctx, base, ") != std::string_view::npos && t.ends_with(");"))) {
      s.kind = LocalStmt::Trap;
      NamedRegisters(t, index, s.use);
      continue;
    }
    if (t == "return;") {
      s.kind = LocalStmt::Return;
      continue;
    }
    if (t.starts_with("if (") && t.ends_with(") return;")) {
      s.kind = LocalStmt::CondReturn;
      NamedRegisters(t, index, s.use);
      continue;
    }
    if (t.starts_with("goto loc_") && t.ends_with(";")) {
      s.kind = LocalStmt::Goto;
      s.target = std::string(t.substr(5, t.size() - 6));
      continue;
    }
    if (t.starts_with("if (") && t.find(") goto loc_") != std::string_view::npos && t.ends_with(";")) {
      s.kind = LocalStmt::CondGoto;
      size_t g = t.find(") goto loc_");
      s.target = std::string(t.substr(g + 7, t.size() - g - 8));
      NamedRegisters(t.substr(0, g), index, s.use);
      continue;
    }
    if (t.starts_with("switch (") && t.ends_with(") {")) {
      s.kind = LocalStmt::Switch;
      NamedRegisters(t, index, s.use);
      openStack.push_back(i);
      continue;
    }
    if (t.ends_with("{")) {
      // if (...) { / { / a block of the vector helpers' bodies
      s.kind = (t.starts_with("if (") || t == "{") ? LocalStmt::IfOpen : LocalStmt::Plain;
      if (t != "{" && !t.starts_with("if ("))
        return LocalizeSkip::Unrecognised;
      NamedRegisters(t, index, s.use);
      openStack.push_back(i);
      continue;
    }
    if (t.starts_with("case ") || t == "default:") {
      s.kind = LocalStmt::Plain;
      continue;
    }
    if (t.starts_with("else") || hasWholeCtx(t) || t.find("return") != std::string_view::npos ||
        t.find("ppc_") != std::string_view::npos || t.find("goto ") != std::string_view::npos)
      return LocalizeSkip::Unrecognised;

    // A plain statement: reads, writes, kills.
    s.kind = LocalStmt::Plain;
    RegSet named;
    NamedRegisters(t, index, named);
    if (named.none())
      continue;
    bool classified = false;
    // Memory stores and dcbz only read registers.
    if (t.starts_with("REX_STORE_U") || t.starts_with("REX_SSTORE_U") || t.starts_with("memset(") ||
        t.starts_with("simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(")) {
      s.use |= named;
      continue;
    }
    // --reg.field;  ++reg.field;  and  x op= rhs  (compound): read and write their target
    if ((t.starts_with("--") || t.starts_with("++")) && t.ends_with(";")) {
      s.use |= named;
      s.dirty |= named;
      continue;
    }
    for (std::string_view op : {" += ", " -= ", " |= ", " &= ", " ^= ", " *= ", " <<= ", " >>= "}) {
      size_t q = t.find(op);
      if (q == std::string_view::npos || classified)
        continue;
      RegSet lhsRegs;
      NamedRegisters(t.substr(0, q), index, lhsRegs);
      s.use |= named;
      s.dirty |= lhsRegs;
      classified = true;
    }
    if (classified)
      continue;
    // <reg>.<field>... = rhs;   (not ==)
    size_t eq = t.find(" = ");
    if (eq != std::string_view::npos && t.ends_with(";")) {
      std::string_view lhs = t.substr(0, eq);
      std::string_view rhs = t.substr(eq + 3);
      size_t dot = lhs.find('.');
      RegSet lhsRegs;
      NamedRegisters(lhs, index, lhsRegs);
      if (lhsRegs.none()) {
        s.use |= named;  // an assignment to a temporary (temp, ea, a vector helper's local): reads only
        classified = true;
      } else if (dot != std::string_view::npos) {
        auto it = index.find(std::string(lhs.substr(0, dot)));
        if (it != index.end() && lhsRegs.count() == 1 && lhsRegs.test(it->second)) {
          const size_t r = it->second;
          RegSet rhsRegs;
          NamedRegisters(rhs, index, rhsRegs);
          s.use |= rhsRegs;
          s.dirty.set(r);
          bool full = false;
          for (auto f : kFull)
            if (t.compare(dot, f.size(), f) == 0)
              full = true;
          if (full && !rhsRegs.test(r))
            s.kill.set(r);
          else
            s.use.set(r);  // a partial write keeps the rest of the old value
          classified = true;
        }
      }
    }
    // crN.compare<...>(a, b, xer);  writes all of crN, reads the arguments.
    if (!classified && t.starts_with("cr") && t.find(".compare") != std::string_view::npos) {
      size_t dot = t.find('.');
      auto it = index.find(std::string(t.substr(0, dot)));
      if (it != index.end()) {
        RegSet args;
        NamedRegisters(t.substr(dot), index, args);
        s.use |= args;
        s.dirty.set(it->second);
        if (!args.test(it->second))
          s.kill.set(it->second);
        classified = true;
      }
    }
    // simde_mm_store_si128((simde__m128i*)vN.u8, value);  simde_mm_store_ps(vN.f32, value);
    if (!classified) {
      std::string_view rest;
      if (t.starts_with("simde_mm_store_si128((simde__m128i*)"))
        rest = t.substr(36);
      else if (t.starts_with("simde_mm_store_ps("))
        rest = t.substr(18);
      if (!rest.empty() && rest[0] == 'v') {
        size_t dot = rest.find('.');
        size_t comma = rest.find(", ");
        auto it = dot == std::string_view::npos ? index.end() : index.find(std::string(rest.substr(0, dot)));
        if (it != index.end() && comma != std::string_view::npos && comma > dot &&
            rest.substr(dot, comma - dot).find_first_of("[(") == std::string_view::npos) {
          RegSet value;
          NamedRegisters(rest.substr(comma), index, value);
          s.use |= value;
          s.dirty.set(it->second);
          if (!value.test(it->second))
            s.kill.set(it->second);
          classified = true;
        }
      }
    }
    if (!classified) {  // unknown shape: everything it names may be read and written
      s.use |= named;
      s.dirty |= named;
    }
  }
  if (!openStack.empty())
    return LocalizeSkip::Unrecognised;

  // 3. Control flow.
  std::unordered_map<size_t, size_t> closeOf;
  for (auto [o, c] : blocks)
    closeOf[o] = c;
  const size_t exitNode = st.size();
  for (size_t i = 0; i < st.size(); ++i) {
    auto& s = st[i];
    auto next = [&]() { return i + 1 < st.size() ? i + 1 : exitNode; };
    switch (s.kind) {
      case LocalStmt::Goto:
      case LocalStmt::CondGoto: {
        auto it = labels.find(s.target);
        if (it == labels.end())
          return LocalizeSkip::Unrecognised;
        s.succ.push_back(it->second);
        if (s.kind == LocalStmt::CondGoto)
          s.succ.push_back(next());
        break;
      }
      case LocalStmt::Return:
      case LocalStmt::TailCall:
        if (s.kind == LocalStmt::TailCall)
          s.succ.push_back(i + 1);  // its return
        break;
      case LocalStmt::CondReturn:
        s.succ.push_back(next());
        break;
      case LocalStmt::IfOpen:
        s.succ.push_back(next());
        if (st[i].text.find("if (") != std::string_view::npos)
          s.succ.push_back(closeOf[i] + 1 < st.size() ? closeOf[i] + 1 : exitNode);
        break;
      case LocalStmt::Switch: {
        // every case body starts on the line after its label
        size_t c = closeOf[i];
        for (size_t k = i + 1; k < c; ++k) {
          std::string_view t = st[k].text;
          size_t ind = 0;
          while (ind < t.size() && t[ind] == '\t')
            ++ind;
          t = t.substr(ind);
          if (t.starts_with("case ") || t == "default:")
            s.succ.push_back(k + 1);
        }
        if (s.succ.empty())
          s.succ.push_back(next());
        break;
      }
      default:
        s.succ.push_back(next());
        break;
    }
  }

  // 4. Dataflow. Dirty: forward, may (union); a call / trap leaves nothing dirty (all written back).
  const size_t n = st.size();
  std::vector<RegSet> dIn(n + 1), dOut(n + 1);
  std::vector<std::vector<size_t>> pred(n + 1);
  for (size_t i = 0; i < n; ++i)
    for (size_t s : st[i].succ)
      pred[s].push_back(i);
  auto syncs = [&](const LocalStmt& s) {
    return s.kind == LocalStmt::Call || s.kind == LocalStmt::TailCall || s.kind == LocalStmt::Trap;
  };
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      RegSet in;
      for (size_t p : pred[i])
        in |= dOut[p];
      RegSet out = syncs(st[i]) && st[i].kind != LocalStmt::Trap ? RegSet() : (in | st[i].dirty);
      if (in != dIn[i] || out != dOut[i]) {
        dIn[i] = in;
        dOut[i] = out;
        changed = true;
      }
    }
  }
  // Needed: backward, may. A register is needed where it may be read before being fully overwritten,
  // including by a write-back: so a register a later write-back may store is never a stale local (it
  // is loaded at entry or reloaded after the call before, on every path).
  for (size_t i = 0; i < n; ++i) {
    auto k = st[i].kind;
    if (k == LocalStmt::Call || k == LocalStmt::TailCall || k == LocalStmt::Trap || k == LocalStmt::Return ||
        k == LocalStmt::CondReturn || (k == LocalStmt::Close && i + 2 >= n && st[i].text == "}"))
      st[i].use |= dIn[i];
  }
  std::vector<RegSet> lIn(n + 1), lOut(n + 1);
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t k = n; k-- > 0;) {
      RegSet out;
      for (size_t s : st[k].succ)
        out |= lIn[s];
      RegSet in = st[k].use | (out & ~st[k].kill);
      if (in != lIn[k] || out != lOut[k]) {
        lIn[k] = in;
        lOut[k] = out;
        changed = true;
      }
    }
  }

  // 5. Emit.
  auto writeBack = [&](const RegSet& set) {
    std::string out;
    for (size_t r = 0; r < order.size(); ++r)
      if (set.test(r))
        out += fmt::format(" ctx.{0} = {0};", order[r]);
    return out;
  };
  auto reload = [&](const RegSet& set) {
    std::string out;
    for (size_t r = 0; r < order.size(); ++r)
      if (set.test(r))
        out += fmt::format(" {0} = ctx.{0};", order[r]);
    return out;
  };
  std::string out;
  out.reserve(renamed.size() + renamed.size() / 8);
  size_t sync = 0;
  for (size_t i = 0; i < n; ++i) {
    const auto& s = st[i];
    std::string_view l = s.text;
    size_t ind = 0;
    while (ind < l.size() && l[ind] == '\t')
      ++ind;
    std::string_view indent = l.substr(0, ind);
    std::string_view t = l.substr(ind);
    std::string line;
    switch (s.kind) {
      case LocalStmt::Call: {
        ++sync;
        std::string so = writeBack(dIn[i]), si = reload(lOut[i]);
        line = so.empty() ? std::string(l) : fmt::format("{}{{{} }}\n{}", indent, so, l);
        if (!si.empty())
          line += fmt::format("\n{}{{{} }}", indent, si);
        break;
      }
      case LocalStmt::TailCall: {
        ++sync;
        std::string so = writeBack(dIn[i]);
        line = so.empty() ? std::string(l) : fmt::format("{}{{{} }}\n{}", indent, so, l);
        break;
      }
      case LocalStmt::Trap: {
        ++sync;
        std::string so = writeBack(dIn[i]), si = reload(lOut[i]);
        if (t.starts_with("if (")) {
          size_t q = t.find(") ppc_trap(ctx, base, ");
          line = fmt::format("{}{}{{{} {}{} }}", indent, t.substr(0, q + 2), so, t.substr(q + 2), si);
        } else {
          line = fmt::format("{}{{{} {}{} }}", indent, so, t, si);
        }
        break;
      }
      case LocalStmt::Return: {
        if (i > 0 && st[i - 1].kind == LocalStmt::TailCall) {
          line = std::string(l);  // the callee left the context as our caller sees it
        } else {
          std::string so = writeBack(dIn[i]);
          line = so.empty() ? std::string(l) : fmt::format("{}{{{} return; }}", indent, so);
        }
        break;
      }
      case LocalStmt::CondReturn: {
        std::string so = writeBack(dIn[i]);
        line = so.empty() ? std::string(l)
                          : fmt::format("{}{}{{{} return; }}", indent, t.substr(0, t.size() - 7), so);
        break;
      }
      case LocalStmt::Close:
        if (ind == 0 && i + 2 >= n) {  // the function's closing brace: falling off the end
          std::string so = writeBack(dIn[i]);
          line = so.empty() ? std::string(l) : fmt::format("\t{{{} }}\n}}", so);
        } else {
          line = std::string(l);
        }
        break;
      default:
        line = std::string(l);
        break;
    }
    out += line;
    if (i + 1 < n)
      out += '\n';
  }
  if (trailingNewline)
    out += '\n';

  // 6. Declarations: the registers read before written are loaded; the rest start zeroed.
  auto typeOf = [](const std::string& nm) -> const char* {
    if (nm == "xer")
      return "PPCXERRegister";
    if (nm.starts_with("cr"))
      return "PPCCRRegister";
    if (nm[0] == 'v')
      return "PPCVRegister";
    return "PPCRegister";
  };
  res.decls.clear();
  for (size_t r = 0; r < order.size(); ++r) {
    if (lIn[0].test(r))
      res.decls += fmt::format("\t{} {} = ctx.{};\n", typeOf(order[r]), order[r], order[r]);
    else
      res.decls += fmt::format("\t{} {}{{}};\n", typeOf(order[r]), order[r]);
  }
  res.macros.clear();
  res.body = std::move(out);
  res.registers = order.size();
  res.syncPoints = sync;
  return LocalizeSkip::None;
}
