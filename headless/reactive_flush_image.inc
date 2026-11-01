        // A copy for a guest read (headless/reactive_flush_log.h): each image of its area, whether it
        // is copied back, how recent its last GPU write is, and where its GPU address leads now.
        if (const u64 eden_flush = ::Eden::ReactiveFlushLog::Active(); eden_flush != 0) {
            const std::optional<DAddr> eden_now =
                gpu_memory != nullptr ? gpu_memory->GpuToCpuAddress(image.gpu_addr) : std::nullopt;
            const std::string eden_leads = !eden_now                      ? std::string{"nothing"}
                                           : *eden_now == image.cpu_addr ? std::string{"its memory"}
                                                                          : fmt::format("{:#x}", *eden_now);
            const std::string eden_line = fmt::format(
                "EDEN_REACTIVE_FLUSH_IMAGE #{} gpu {:#x} memory [{:#x}, {:#x}) {}x{}x{} format {} type {} "
                "levels {} layers {} flags {:#x}, {}; GPU write {} of {}; its GPU address leads to {}",
                eden_flush, image.gpu_addr, image.cpu_addr, image.cpu_addr_end, image.info.size.width,
                image.info.size.height, image.info.size.depth, static_cast<int>(image.info.format),
                static_cast<int>(image.info.type), image.info.resources.levels, image.info.resources.layers,
                static_cast<u32>(image.flags), IsDownloadable(image) ? "copied back" : "not copied back",
                image.modification_tick, modification_tick, eden_leads);
            LOG_WARNING(HW_GPU, "{}", eden_line);
            if (::Eden::ReactiveFlushLog::ToStderr(eden_flush)) std::fprintf(stderr, "%s\n", eden_line.c_str());
        }
