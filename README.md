# OperandPrefixProbe

Messing around with the `0x66` operand-size override prefix on x86-64.

A small Windows experiment written in C, comparing a relative `JMP` instruction with and without the prefix.

Both variants run independently in separate child processes, and their resulting control flow or exception is logged.

## Build target

Windows x64. Visual Studio with the v145 C/C++ toolset and Windows SDK.

Open `OperandPrefixProbe.slnx` and build Debug or Release x64.

## Usage

```powershell
.\x64\Release\OperandPrefixProbe.exe
```

Runs both variants and prints a summary. To run one variant directly, use `--probe baseline` or `--probe prefixed`.

The results describe the current CPU and environment; they do not identify a CPU vendor or virtual machine.
