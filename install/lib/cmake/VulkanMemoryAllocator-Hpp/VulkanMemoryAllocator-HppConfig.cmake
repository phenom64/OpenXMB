include (CMakeFindDependencyMacro)

find_dependency (Vulkan REQUIRED)
find_dependency (VulkanMemoryAllocator REQUIRED)

include (${CMAKE_CURRENT_LIST_DIR}/VulkanMemoryAllocator-HppTargets.cmake)
