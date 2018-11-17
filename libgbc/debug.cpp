#include "cpu.hpp"
#include "machine.hpp"

namespace gbc
{
  static inline std::vector<std::string>
    split(const std::string& txt, char ch)
  {
      size_t pos = txt.find(ch);
      size_t initialPos = 0;
      std::vector<std::string> strs;

      while (pos != std::string::npos) {
          strs.push_back( txt.substr( initialPos, pos - initialPos ) );
          initialPos = pos + 1;

          pos = txt.find( ch, initialPos );
      }

      // Add the last one
      strs.push_back(txt.substr(initialPos, std::min(pos, txt.size()) - initialPos + 1));
      return strs;
  }

  static void print_help()
  {
    const char* help_text = R"V0G0N(
  usage: command [options]
    commands:
      ?, help               Show this informational text
      c, continue           Continue execution, disable stepping
      s, step [steps=1]     Run [steps] instructions, then break
      v, verbose            Toggle verbose instruction execution
      b, break [addr]       Breakpoint on executing [addr]
      clear                 Clear all breakpoints
      reset                 Reset the machine
      read [addr] (len=1)   Read from [addr] (len) bytes and print
      write [addr] [value]  Write [value] to memory location [addr]
      debug                 Trigger the debug interrupt handler
      vblank                Render current screen and call vblank
)V0G0N";
    printf("%s\n", help_text);
  }

  static bool execute_commands(CPU& cpu)
  {
    printf("Enter = cont, help, quit: ");
    std::string text;
    while (true)
    {
      const int c = getchar(); // press any key
      if (c == '\n' || c < 0) break;
      else text.append(1, (char) c);
    }
    if (text.empty()) return false;
    std::vector<std::string> params = split(text, ' ');
    const auto& cmd = params[0];

    // continue
    if (cmd == "c" || cmd == "continue") {
      cpu.break_on_steps(0);
      return false;
    }
    // stepping
    if (cmd == "") {
      return false;
    }
    else if (cmd == "s" || cmd == "step") {
      cpu.machine().verbose_instructions = true; // ???
      int steps = 1;
      if (params.size() > 1) steps = std::stoi(params[1]);
      printf("Pressing Enter will now execute %d steps\n", steps);
      cpu.break_on_steps(steps);
      return false;
    }
    // breaking
    else if (cmd == "b" || cmd == "break") {
      if (params.size() < 2) {
        printf(">>> Not enough parameters: break [addr]\n");
        return true;
      }
      unsigned long hex = std::strtoul(params[1].c_str(), 0, 16);
      cpu.default_pausepoint(hex & 0xFFFF);
      return true;
    }
    else if (cmd == "clear") {
      cpu.breakpoints().clear();
      return true;
    }
    // verbose instructions
    else if (cmd == "v" || cmd == "verbose") {
      bool& v = cpu.machine().verbose_instructions;
      v = !v;
      printf("Verbose instructions are now %s\n", v ? "ON" : "OFF");
      return true;
    }
    else if (cmd == "r" || cmd == "run") {
      cpu.machine().verbose_instructions = false;
      cpu.break_on_steps(0);
      return false;
    }
    else if (cmd == "q" || cmd == "quit" || cmd == "exit") {
      cpu.stop();
      return false;
    }
    else if (cmd == "reset") {
      cpu.machine().reset();
      cpu.break_now();
      return false;
    }
    // read 0xAddr size
    else if (cmd == "ld" || cmd == "read")
    {
      if (params.size() < 2) {
        printf(">>> Not enough parameters: read [addr] (length=1)\n");
        return true;
      }
      unsigned long hex = std::strtoul(params[1].c_str(), 0, 16);
      int bytes = 1;
      if (params.size() > 2) bytes = std::stoi(params[2]);
      int col = 0;
      for (int i = 0; i < bytes; i++) {
        if (col == 0) printf("0x%04lx: ", hex+i);
        printf("0x%02x ", cpu.memory().read8(hex+i));
        if (++col == 4) { printf("\n"); col = 0; }
      }
      if (col) printf("\n");
      return true;
    }
    // write 0xAddr value
    else if (cmd == "write")
    {
      if (params.size() < 3) {
        printf(">>> Not enough parameters: write [addr] [value]\n");
        return true;
      }
      unsigned long hex = std::strtoul(params[1].c_str(), 0, 16);
      int value = std::stoi(params[2]) & 0xff;
      printf("0x%04lx -> 0x%02x\n", hex, value);
      cpu.memory().write8(hex, value);
      return true;
    }
    else if (cmd == "vblank") {
      cpu.machine().gpu.render_and_vblank();
      return true;
    }
    else if (cmd == "debug") {
      auto& io = cpu.machine().io;
      io.debugint.callback(cpu.machine(), io.debugint);
      io.debugint.last_time = cpu.gettime();
      return true;
    }
    else if (cmd == "help" || cmd == "?") {
      print_help();
      return true;
    }
    else {
      printf(">>> Unknown command: '%s'\n", cmd.c_str());
      print_help();
      return true;
    }
    return false;
  }

  void CPU::print_and_pause(CPU& cpu, const uint8_t opcode)
  {
    char buffer[512];
    cpu.decode(opcode).printer(buffer, sizeof(buffer), cpu, opcode);
    printf("\n");
    printf(">>> Breakpoint at [pc 0x%04x] opcode 0x%02x: %s\n",
           cpu.registers().pc, opcode, buffer);
    // CPU registers
    printf("%s\n", cpu.registers().to_string().c_str());
    // I/O interrupt registers
    auto& io = cpu.machine().io;
    printf("\tIF = 0x%02x  IE = 0x%02x  IME 0x%x\n",
           io.read_io(IO::REG_IF), io.read_io(IO::REG_IE), cpu.ime());
    try {
      auto& mem = cpu.memory();
      printf("\t(HL) = 0x%02x  (SP) = 0x%04x\n",
            cpu.read_hl(), mem.read16(cpu.registers().sp));
    } catch (...) {
      printf("\tUnable to read from (HL) or (SP)\n");
    }
    while (execute_commands(cpu));
  } // print_and_pause(...)

  bool CPU::break_time() const
  {
    if (UNLIKELY(this->m_break)) return true;
    if (UNLIKELY(m_break_steps_cnt != 0)) {
      m_break_steps--;
      if (m_break_steps <= 0) {
        m_break_steps = m_break_steps_cnt;
        return true;
      }
    }
    return false;
  }

  void CPU::break_on_steps(int steps)
  {
    assert(steps >= 0);
    this->m_break_steps_cnt = steps;
    this->m_break_steps = steps;
  }

  void CPU::break_checks()
  {
    if (UNLIKELY(this->break_time())) {
      this->m_break = false;
      // pause for each instruction
      this->print_and_pause(*this, this->readop8(0));
      // user can quit during break
      if (!this->is_running()) return;
    }
    else if (UNLIKELY(!m_breakpoints.empty())) {
      // look for breakpoints
      auto it = m_breakpoints.find(registers().pc);
      if (it != m_breakpoints.end()) {
        auto& bp = it->second;
        bp.callback(*this, this->readop8(0));
        // user can quit during break
        if (!this->is_running()) return;
      }
    }
  }
}
