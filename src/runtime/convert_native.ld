SECTIONS
{
    . = 0;
    .text.ssharp_convert_native :
    {
        *(.text.ssharp_convert_native)
        *(.rodata)
        *(.rodata.*)
    }
}
