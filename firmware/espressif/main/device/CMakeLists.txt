# Device contracts and façades are platform-independent. Platform supplies
# their implementations to the application composition root.
list(APPEND micropixel_sources
    "device/device_registry.cpp"
    "${micropixel_shared_host}/device/device_services.cpp"
    "${micropixel_shared_host}/device/text.cpp"
)
