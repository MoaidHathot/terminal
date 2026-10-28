#Requires -Version 7.0
<#
.SYNOPSIS
    Exercises Sixel output through the built control's public C API in isolated processes.
.DESCRIPTION
    Runs opaque and composed hosts without showing a window. The release DLL must survive
    each image and still answer a terminal query. A native fail-fast must fail the build,
    including optimizer/hardening failures which cannot be caught by managed exceptions.
#>
[CmdletBinding()]
param(
    [string] $NativeDll = (Join-Path $PSScriptRoot '..\..\bin\x64\Release\Microsoft.Terminal.Control\Microsoft.Terminal.Control.dll'),
    [switch] $Child,
    [ValidateSet(0, 1)] [int] $Flags = 0
)

$ErrorActionPreference = 'Stop'
$NativeDll = (Resolve-Path -LiteralPath $NativeDll).Path
if (-not $Child) {
    foreach ($mode in 0, 1) {
        $start = [Diagnostics.ProcessStartInfo]::new((Get-Command pwsh).Source)
        $start.UseShellExecute = $false
        foreach ($argument in @('-NoProfile', '-Sta', '-File', $PSCommandPath, '-NativeDll', $NativeDll, '-Child', '-Flags', "$mode")) {
            $start.ArgumentList.Add($argument)
        }
        $process = [Diagnostics.Process]::Start($start)
        try {
            if (-not $process.WaitForExit(60000)) {
                $process.Kill($true)
                throw "Sixel probe timed out (flags=$mode)."
            }
            if ($process.ExitCode -ne 0) {
                throw ('Sixel probe failed (flags={0}, exit=0x{1:X8}).' -f $mode, $process.ExitCode)
            }
        } finally { $process.Dispose() }
    }
    Write-Output 'Sixel regression: opaque and composed hosts passed.'
    exit 0
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SixelRegression
{
    [StructLayout(LayoutKind.Sequential)] public struct Size { public int Width, Height; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowExW(uint exStyle, string className, string title, uint style, int x, int y, int width, int height, IntPtr parent, IntPtr menu, IntPtr instance, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool DestroyWindow(IntPtr hwnd);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int CreateTerminal(IntPtr parent, uint flags, out IntPtr hwnd, out IntPtr terminal);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int ResizeTerminal(IntPtr terminal, int width, int height, out Size size);
    [UnmanagedFunctionPointer(CallingConvention.StdCall, CharSet = CharSet.Unicode)] private delegate void SendOutput(IntPtr terminal, [MarshalAs(UnmanagedType.LPWStr)] string data);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate void WriteCallback(IntPtr text);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate void RegisterWrite(IntPtr terminal, WriteCallback callback);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate void Destroy(IntPtr terminal);

    public static void Run(string path, uint flags)
    {
        // Keep the module loaded until process exit: COM may cache objects implemented by it.
        var module = NativeLibrary.Load(path);
        var create = Marshal.GetDelegateForFunctionPointer<CreateTerminal>(NativeLibrary.GetExport(module, "CreateTerminalEx"));
        var resize = Marshal.GetDelegateForFunctionPointer<ResizeTerminal>(NativeLibrary.GetExport(module, "TerminalTriggerResize"));
        var send = Marshal.GetDelegateForFunctionPointer<SendOutput>(NativeLibrary.GetExport(module, "TerminalSendOutput"));
        var register = Marshal.GetDelegateForFunctionPointer<RegisterWrite>(NativeLibrary.GetExport(module, "TerminalRegisterWriteCallback"));
        var destroy = Marshal.GetDelegateForFunctionPointer<Destroy>(NativeLibrary.GetExport(module, "DestroyTerminal"));
        var parent = CreateWindowExW(0, "STATIC", "Sixel regression", 0x80000000, 0, 0, 800, 600, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        if (parent == IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        var replies = new StringBuilder();
        WriteCallback callback = text =>
        {
            try { replies.Append(Marshal.PtrToStringUni(text)); }
            finally { Marshal.FreeCoTaskMem(text); }
        };
        IntPtr terminal = IntPtr.Zero;
        try
        {
            Marshal.ThrowExceptionForHR(create(parent, flags, out var hwnd, out terminal));
            Marshal.ThrowExceptionForHR(resize(terminal, 800, 600, out var size));
            register(terminal, callback);
            Console.WriteLine($"Sixel host flags={flags}, grid={size.Width}x{size.Height}");

            void Check(string name, params string[] chunks)
            {
                Console.WriteLine("RUN   " + name);
                foreach (var chunk in chunks) send(terminal, chunk);
                replies.Clear();
                send(terminal, "\x1b[5n");
                if (!replies.ToString().Contains("\x1b[0n")) throw new InvalidOperationException("No device-status reply after " + name);
                Console.WriteLine("PASS  " + name);
            }

            Check("one-column red image", "\x1bP0;1q#1;2;100;0;0~\x1b\\");
            Check("opaque background fill", "\x1bP0;0q\"1;1;10;20?\x1b\\");
            Check("transparent image", "\x1bP0;1q\"1;1;10;20?\x1b\\");

            var palette = new StringBuilder("\x1bP0;1q\"1;1;256;6");
            for (var i = 0; i < 256; i++) palette.Append($"#{i};2;{i % 101};{(i * 3) % 101};{(i * 7) % 101}");
            for (var i = 0; i < 256; i++) palette.Append($"#{i}~");
            palette.Append("\x1b\\");
            Check("all 256 palette entries", palette.ToString());
            Check("fragmented image and palette redefinition", "\x1bP0;1q#255;2;0;100;0", "!20~$#255;2;0;0;100", "!20~", "\x1b", "\\");
            Check("aspect ratio and scrolling", "\x1b[?80l\x1b[999;1H\x1bP0;1q\"3;1;20;36#0;2;100;0;0!20~-!20~\x1b\\");
            Check("alternate screen", "\x1b[?1049h\x1b[H\x1bP0;1q#255;2;0;0;100!20~\x1b\\\x1b[?1049l");
            Marshal.ThrowExceptionForHR(resize(terminal, 640, 480, out size));
            Check("image after resize", "\x1b[H\x1bP0;1q#0;2;100;100;100~\x1b\\");
        }
        finally
        {
            if (terminal != IntPtr.Zero) destroy(terminal);
            DestroyWindow(parent);
            GC.KeepAlive(callback);
        }
    }
}
'@
[SixelRegression]::Run($NativeDll, [uint32]$Flags)
