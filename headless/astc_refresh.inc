    if (True(image.flags & ImageFlagBits::AsynchronousDecode) &&
        ::Eden::AstcPolicy::InBackground(MapSizeBytes(image))) {
        const auto input_bytes = static_cast<unsigned long long>(image.unswizzled_size_bytes);
        const auto output_bytes = static_cast<unsigned long long>(MapSizeBytes(image));
        const auto payload = ::Eden::AstcPolicy::DecodePayload(
            input_bytes, output_bytes, image.info.size.width, image.info.size.height);
        auto reservation = payload ? astc_decode_budget.Reserve(*payload, ::Eden::AstcPolicy::Busy())
                                   : std::nullopt;
        if (!::Eden::AstcPolicy::Busy() && ::Eden::AstcPolicy::ReportLightBackground()) {
            std::printf("EDEN_ASTC_BACKGROUND texels=%u size=%ux%u layers=%u levels=%u draws=%u "
                        "count=%u admitted=%d input=%llu output=%llu payload=%llu pending=%llu\n",
                        MapSizeBytes(image), image.info.size.width, image.info.size.height,
                        static_cast<unsigned>(image.info.resources.layers),
                        static_cast<unsigned>(image.info.resources.levels),
                        ::Eden::AstcPolicy::Draws(), ::Eden::AstcPolicy::light_background,
                        int(reservation.has_value()), input_bytes, output_bytes,
                        static_cast<unsigned long long>(payload.value_or(0)),
                        static_cast<unsigned long long>(astc_decode_budget.Held()));
        }
        if (reservation) {
            QueueAsyncDecode(image, image_id, std::move(*reservation));
            return;
        }
    }
