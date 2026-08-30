set confirm off
set pagination off
set disassembly-flavor att
file build/kernel.elf
target remote :1234

define ksyms-info
    printf "ksyms: %p .. %p\n", &__ksyms_start, &__ksyms_end
end
document ksyms-info
Print the bounds of the embedded kernel symbol table.
end

define pmm-stats
    printf "pmm: total=%lu free=%lu max_pfn=%lu\n", pmm_stats.total_pages, pmm_stats.free_pages, pmm_max_pfn
    set $o = 0
    while $o <= 10
        printf "  order %2d: %lu blocks\n", $o, pmm_free_count[$o]
        set $o = $o + 1
    end
end
document pmm-stats
Dump buddy allocator free list counts.
end
