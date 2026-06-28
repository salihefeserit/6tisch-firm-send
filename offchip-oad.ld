/*
 * CC13x2/CC26x2 off-chip OAD application linker script.
 *
 * Layout:
 *   0x00000000 - 0x000000A7  TI imgHdr_t in .oad_image_header
 *   0x000000A8 - 0x00055FFF  application image copied by off-chip BIM
 *
 * The application CCFG is discarded because the BIM image owns the final CCFG.
 */

MIN_STACKSIZE = 0x600;
HEAPSIZE      = 0x100;

MEMORY
{
    FLASH_HEADER (RX) : ORIGIN = 0x00000000, LENGTH = 0x000000A8
    FLASH_APP    (RX) : ORIGIN = 0x000000A8, LENGTH = 0x00056000 - 0x000000A8
    SRAM         (RWX): ORIGIN = 0x20000000, LENGTH = 0x00014000
    GPRAM        (RWX): ORIGIN = 0x11000000, LENGTH = 0x00002000
}

REGION_ALIAS("REGION_TEXT",      FLASH_APP);
REGION_ALIAS("REGION_BSS",       SRAM);
REGION_ALIAS("REGION_DATA",      SRAM);
REGION_ALIAS("REGION_STACK",     SRAM);
REGION_ALIAS("REGION_HEAP",      SRAM);
REGION_ALIAS("REGION_ARM_EXIDX", FLASH_APP);
REGION_ALIAS("REGION_ARM_EXTAB", FLASH_APP);

SECTIONS {

    .oad_image_header ORIGIN(FLASH_HEADER) :
    {
        KEEP(*(.oad_image_header))
        . = ALIGN(4);
        ASSERT(. <= ORIGIN(FLASH_HEADER) + LENGTH(FLASH_HEADER), "OAD image header exceeds reserved 0xA8-byte area");
    } > FLASH_HEADER

    .resetVecs ORIGIN(FLASH_APP) : ALIGN(4)
    {
        _vectors = .;
        KEEP(*(.resetVecs))
    } > REGION_TEXT

    .ramVecs (NOLOAD) : ALIGN(1024) {
        KEEP (*(.ramVecs))
    } > REGION_DATA

    /*
     * UDMACC26XX_CONFIG_BASE below must match UDMACC26XX_CONFIG_BASE defined
     * by ti/drivers/dma/UDMACC26XX.h
     */
    UDMACC26XX_CONFIG_BASE = 0x20001800;

    DMA_SPI0_RX_CTEA   = (UDMACC26XX_CONFIG_BASE + 0x30);
    DMA_SPI0_TX_CTEA   = (UDMACC26XX_CONFIG_BASE + 0x40);
    DMA_ADC_PRI_CTEA   = (UDMACC26XX_CONFIG_BASE + 0x70);
    DMA_GPT0A_PRI_CTEA = (UDMACC26XX_CONFIG_BASE + 0x90);
    DMA_SPI1_RX_CTEA   = (UDMACC26XX_CONFIG_BASE + 0x100);
    DMA_SPI1_TX_CTEA   = (UDMACC26XX_CONFIG_BASE + 0x110);

    DMA_SPI0_RX_ALT_CTEA = (UDMACC26XX_CONFIG_BASE + 0x230);
    DMA_SPI0_TX_ALT_CTEA = (UDMACC26XX_CONFIG_BASE + 0x240);
    DMA_ADC_ALT_CTEA     = (UDMACC26XX_CONFIG_BASE + 0x270);
    DMA_GPT0A_ALT_CTEA   = (UDMACC26XX_CONFIG_BASE + 0x290);
    DMA_SPI1_RX_ALT_CTEA = (UDMACC26XX_CONFIG_BASE + 0x300);
    DMA_SPI1_TX_ALT_CTEA = (UDMACC26XX_CONFIG_BASE + 0x310);

    UDMACC26XX_dmaSpi0RxControlTableEntry_is_placed = 0;
    .dmaSpi0RxControlTableEntry DMA_SPI0_RX_CTEA (NOLOAD) : AT (DMA_SPI0_RX_CTEA) {
        *(.dmaSpi0RxControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaSpi0TxControlTableEntry_is_placed = 0;
    .dmaSpi0TxControlTableEntry DMA_SPI0_TX_CTEA (NOLOAD) : AT (DMA_SPI0_TX_CTEA) {
        *(.dmaSpi0TxControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaADCPriControlTableEntry_is_placed = 0;
    .dmaADCPriControlTableEntry DMA_ADC_PRI_CTEA (NOLOAD) : AT (DMA_ADC_PRI_CTEA) {
        *(.dmaADCPriControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaGPT0APriControlTableEntry_is_placed = 0;
    .dmaGPT0APriControlTableEntry DMA_GPT0A_PRI_CTEA (NOLOAD) : AT (DMA_GPT0A_PRI_CTEA) {
        *(.dmaGPT0APriControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaSpi1RxControlTableEntry_is_placed = 0;
    .dmaSpi1RxControlTableEntry DMA_SPI1_RX_CTEA (NOLOAD) : AT (DMA_SPI1_RX_CTEA) {
        *(.dmaSpi1RxControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaSpi1TxControlTableEntry_is_placed = 0;
    .dmaSpi1TxControlTableEntry DMA_SPI1_TX_CTEA (NOLOAD) : AT (DMA_SPI1_TX_CTEA) {
        *(.dmaSpi1TxControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaSpi0RxAltControlTableEntry_is_placed = 0;
    .dmaSpi0RxAltControlTableEntry DMA_SPI0_RX_ALT_CTEA (NOLOAD) : AT (DMA_SPI0_RX_ALT_CTEA) {
        *(.dmaSpi0RxAltControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaSpi0TxAltControlTableEntry_is_placed = 0;
    .dmaSpi0TxAltControlTableEntry DMA_SPI0_TX_ALT_CTEA (NOLOAD) : AT (DMA_SPI0_TX_ALT_CTEA) {
        *(.dmaSpi0TxAltControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaADCAltControlTableEntry_is_placed = 0;
    .dmaADCAltControlTableEntry DMA_ADC_ALT_CTEA (NOLOAD) : AT (DMA_ADC_ALT_CTEA) {
        *(.dmaADCAltControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaGPT0AAltControlTableEntry_is_placed = 0;
    .dmaGPT0AAltControlTableEntry DMA_GPT0A_ALT_CTEA (NOLOAD) : AT (DMA_GPT0A_ALT_CTEA) {
        *(.dmaGPT0AAltControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaSpi1RxAltControlTableEntry_is_placed = 0;
    .dmaSpi1RxAltControlTableEntry DMA_SPI1_RX_ALT_CTEA (NOLOAD) : AT (DMA_SPI1_RX_ALT_CTEA) {
        *(.dmaSpi1RxAltControlTableEntry)
    } > REGION_DATA

    UDMACC26XX_dmaSpi1TxAltControlTableEntry_is_placed = 0;
    .dmaSpi1TxAltControlTableEntry DMA_SPI1_TX_ALT_CTEA (NOLOAD) : AT (DMA_SPI1_TX_ALT_CTEA) {
        *(.dmaSpi1TxAltControlTableEntry)
    } > REGION_DATA

    .text : {
        CREATE_OBJECT_SYMBOLS
        *(.text)
        *(.text.*)
        . = ALIGN(0x4);
        KEEP (*(.ctors))
        . = ALIGN(0x4);
        KEEP (*(.dtors))
        . = ALIGN(0x4);
        __init_array_start = .;
        KEEP (*(.init_array*))
        __init_array_end = .;
        *(.init)
        *(.fini*)
    } > REGION_TEXT AT> REGION_TEXT

    PROVIDE (__etext = .);
    PROVIDE (_etext = .);
    PROVIDE (etext = .);

    .rodata : {
        *(.rodata)
        *(.rodata.*)
    } > REGION_TEXT AT> REGION_TEXT

    .data : ALIGN(0x4) {
        __data_load__ = LOADADDR(.data);
        __data_start__ = .;
        *(.data)
        *(.data.*)
        . = ALIGN (0x4);
        __data_end__ = .;
    } > REGION_DATA AT> REGION_TEXT

    .ARM.exidx : {
        __exidx_start = .;
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        __exidx_end = .;
    } > REGION_ARM_EXIDX AT> REGION_ARM_EXIDX

    .ARM.extab : {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > REGION_ARM_EXTAB AT> REGION_ARM_EXTAB

    .nvs (NOLOAD) : ALIGN(0x2000) {
        *(.nvs)
    } > REGION_TEXT

    PROVIDE(_flash_end = .);

    .bss : {
        __bss_start__ = .;
        *(.shbss)
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(0x4);
        __bss_end__ = .;
    } > REGION_BSS AT> REGION_BSS

    .heap (NOLOAD) : ALIGN(0x4) {
        PROVIDE(__heap_start__ = .);
        PROVIDE(_heap          = __heap_start__);
        PROVIDE(end            = __heap_start__);
        PROVIDE(_end           = __heap_start__);
        PROVIDE(__end          = __heap_start__);

        . += HEAPSIZE;
        KEEP(*(.heap))

        PROVIDE(__heap_end__ = .);
        PROVIDE(_eheap       = __heap_end__);
        PROVIDE(__HeapLimit  = __heap_end__);
    } > REGION_HEAP AT> REGION_HEAP

    PROVIDE(__stack_size = ORIGIN(SRAM) + LENGTH(SRAM) - ALIGN(0x8));

    .stack (NOLOAD) : {
        PROVIDE(_stack        = ALIGN(0x8));
        PROVIDE(__stack       = _stack);

        PROVIDE(_stack_end    = ORIGIN(SRAM) + LENGTH(SRAM));
        PROVIDE(__stack_end   = _stack_end);
        PROVIDE(_stack_origin = _stack_end);

        ASSERT(__stack_size >= MIN_STACKSIZE, "Error: No room left for the stack");
    } > REGION_STACK AT> REGION_STACK

    /DISCARD/ :
    {
        *(.ccfg)
        *(.ccfg.*)
    }
}
