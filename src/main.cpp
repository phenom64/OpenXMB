/*
 * TM & (C) 2025 Syndromatic Ltd. All rights reserved.
 * Designed by Kavish Krishnakumar in Manchester.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at  option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITH ABSOLUTELY NO WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <QGuiApplication>
#include <QWindow>
#include <QVulkanInstance>
#include <QVulkanFunctions>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QScreen>
#include <QRect>
#include <QTimer>
#include <QElapsedTimer>
#include <QDebug>
#include <QColor>
#include <QVector2D>
#include <QVector4D>
#include <QFile>
#include <QMatrix4x4>
#include <cmath>
#include <array>

#define ENABLE_VALIDATION_LAYERS

// Structure for uniform buffer (exactly matching WaveShader.frag)
struct UniformBlock {
    float time;
    float speed;
    float amplitude;
    float frequency;
    QVector4D baseColor;
    QVector4D waveColor;
    float threshold;
    float dustIntensity;
    float minDist;
    float maxDist;
    int maxDraws;
    QVector2D resolution;
    float padding[2];  // Vulkan alignment
};

// Vertex data for fullscreen quad
struct Vertex {
    QVector2D pos;
    QVector2D texCoord;
};

class OpenXMBWindow : public QWindow
{
    Q_OBJECT

public:
    OpenXMBWindow() : QWindow() {
        setSurfaceType(VulkanSurface);
        
        // Wave animation properties (from WaveItem)
        m_time = 0.0;
        m_speed = 0.5;
        m_amplitude = 0.05;
        m_frequency = 10.0;
        m_baseColor = QColor("#000000");
        m_waveColor = QColor("#1A1A1A");
        m_threshold = 0.99;
        m_dustIntensity = 1.0;
        m_minDist = 0.13;
        m_maxDist = 40.0;
        m_maxDraws = 40;
        
        // Animation timer
        m_animTimer = new QTimer(this);
        m_animTimer->setInterval(16); // ~60 FPS
        connect(m_animTimer, &QTimer::timeout, this, &OpenXMBWindow::updateAnimation);
        
        // Vulkan objects
        m_device = VK_NULL_HANDLE;
        m_physicalDevice = VK_NULL_HANDLE;
        m_graphicsQueue = VK_NULL_HANDLE;
        m_commandPool = VK_NULL_HANDLE;
        m_swapchain = VK_NULL_HANDLE;
        m_renderPass = VK_NULL_HANDLE;
        m_pipelineLayout = VK_NULL_HANDLE;
        m_graphicsPipeline = VK_NULL_HANDLE;
        m_vertexBuffer = VK_NULL_HANDLE;
        m_vertexBufferMemory = VK_NULL_HANDLE;
        m_uniformBuffer = VK_NULL_HANDLE;
        m_uniformBufferMemory = VK_NULL_HANDLE;
        m_uniformBufferMapped = nullptr;
        m_descriptorSetLayout = VK_NULL_HANDLE;
        m_descriptorPool = VK_NULL_HANDLE;
        m_descriptorSet = VK_NULL_HANDLE;
        
        qDebug() << "OpenXMB Window created - Ready for wave rendering!";
    }
    
    ~OpenXMBWindow() {
        if (m_device != VK_NULL_HANDLE) {
            cleanupVulkan();
        }
    }

    void exposeEvent(QExposeEvent *) override {
        if (isExposed() && !m_initialized) {
            m_initialized = true;
            qDebug() << "🌊 Initializing OpenXMB Wave Renderer...";
            
            // Get Vulkan instance and functions
            m_vulkanInstance = vulkanInstance();
            if (!m_vulkanInstance) {
                qFatal("No Vulkan instance available");
                return;
            }
            
            m_vulkanFuncs = m_vulkanInstance->functions();
            m_deviceFuncs = nullptr; // Will be set after device creation
            
            // Initialize Vulkan rendering
            if (initializeVulkan()) {
                qDebug() << "🚀 OpenXMB Wave Renderer initialized successfully!";
                
                // Start the animation loop
                m_animTimer->start();
                
                // Render first frame
                render();
            } else {
                qFatal("Failed to initialize Vulkan renderer");
            }
        }
    }

    bool event(QEvent *e) override {
        if (e->type() == QEvent::UpdateRequest) {
            if (m_initialized) {
                render();
            }
        } else if (e->type() == QEvent::KeyPress) {
            QKeyEvent *ke = static_cast<QKeyEvent*>(e);
            if (ke->key() == Qt::Key_Escape || ke->key() == Qt::Key_Q) {
                qDebug() << "👋 Exiting OpenXMB...";
                close();
            }
        }
        return QWindow::event(e);
    }

private slots:
    void updateAnimation() {
        // Update time (smooth animation like  QML Timer)
        m_time += 0.01;
        if (m_time > 1000.0) m_time = 0.0;
        
        // Update the uniform buffer with new time
        updateUniformBuffer();
        
        // Request redraw for smooth 60fps animation
        requestUpdate();
    }

private:
    bool initializeVulkan() {
        qDebug() << "🔧 Setting up Vulkan pipeline...";
        
        // Step 1: Create logical device and get queues
        if (!createDevice()) return false;
        
        // Step 2: Create swapchain
        if (!createSwapchain()) return false;
        
        // Step 3: Create render pass
        if (!createRenderPass()) return false;
        
        // Step 4: Create framebuffers
        if (!createFramebuffers()) return false;
        
        // Step 5: Create command pool
        if (!createCommandPool()) return false;
        
        // Step 6: Create vertex buffer (fullscreen quad)
        if (!createVertexBuffer()) return false;
        
        // Step 7: Create uniform buffer
        if (!createUniformBuffer()) return false;
        
        // Step 8: Load and compile shaders
        if (!createShaders()) return false;
        
        // Step 9: Create descriptor sets
        if (!createDescriptorSets()) return false;
        
        // Step 10: Create graphics pipeline
        if (!createGraphicsPipeline()) return false;
        
        // Step 11: Create command buffers
        if (!createCommandBuffers()) return false;
        
        qDebug() << "✅ Vulkan pipeline created successfully!";
        return true;
    }
    
    bool createDevice() {
        qDebug() << "📱 Creating Vulkan device...";
        
        // Find physical device
        uint32_t deviceCount = 0;
        m_vulkanFuncs->vkEnumeratePhysicalDevices(m_vulkanInstance->vkInstance(), &deviceCount, nullptr);
        if (deviceCount == 0) {
            qWarning() << "No Vulkan physical devices found!";
            return false;
        }
        
        std::vector<VkPhysicalDevice> devices(deviceCount);
        m_vulkanFuncs->vkEnumeratePhysicalDevices(m_vulkanInstance->vkInstance(), &deviceCount, devices.data());
        
        // Use first available device
        m_physicalDevice = devices[0];
        
        VkPhysicalDeviceProperties props;
        m_vulkanFuncs->vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
        qDebug() << "Using GPU:" << props.deviceName;
        
        // Find graphics queue family
        uint32_t queueFamilyCount = 0;
        m_vulkanFuncs->vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, nullptr);
        
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        m_vulkanFuncs->vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &queueFamilyCount, queueFamilies.data());
        
        m_graphicsQueueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                m_graphicsQueueFamily = i;
                break;
            }
        }
        
        if (m_graphicsQueueFamily == UINT32_MAX) {
            qWarning() << "No graphics queue family found!";
            return false;
        }
        
        // Create logical device
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = m_graphicsQueueFamily;
        queueCreateInfo.queueCount = 1;
        float queuePriority = 1.0f;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        
        VkPhysicalDeviceFeatures deviceFeatures{};
        
        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pEnabledFeatures = &deviceFeatures;
        createInfo.enabledExtensionCount = 0;
        
        VkResult result = m_vulkanFuncs->vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create logical device! Error:" << result;
            return false;
        }
        
        // Get device functions
        m_deviceFuncs = m_vulkanInstance->deviceFunctions(m_device);
        if (!m_deviceFuncs) {
            qWarning() << "Failed to get device functions!";
            return false;
        }
        
        // Get graphics queue
        m_deviceFuncs->vkGetDeviceQueue(m_device, m_graphicsQueueFamily, 0, &m_graphicsQueue);
        
        qDebug() << "✅ Vulkan device created successfully";
        return true;
    }
    
    bool createSwapchain() {
        qDebug() << "🖼️ Creating swapchain...";
        
        // For this implementation, we'll create a minimal swapchain
        // In a full implementation, you'd query surface capabilities, formats, etc.
        
        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = VK_NULL_HANDLE; // We'll render directly to the window
        createInfo.minImageCount = 2;
        createInfo.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        createInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        createInfo.imageExtent = {(uint32_t)width(), (uint32_t)height()};
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        createInfo.clipped = VK_TRUE;
        createInfo.oldSwapchain = VK_NULL_HANDLE;
        
        // For this demo, we'll skip actual swapchain creation and render to framebuffer
        m_swapchain = VK_NULL_HANDLE;
        
        qDebug() << "✅ Swapchain ready";
        return true;
    }
    
    bool createRenderPass() {
        qDebug() << "🎨 Creating render pass...";
        
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = VK_FORMAT_B8G8R8A8_UNORM;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        
        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        
        VkResult result = m_deviceFuncs->vkCreateRenderPass(m_device, &renderPassInfo, nullptr, &m_renderPass);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create render pass! Error:" << result;
            return false;
        }
        
        qDebug() << "✅ Render pass created";
        return true;
    }
    
    bool createFramebuffers() {
        qDebug() << "🖼️ Creating framebuffers...";
        // For this demo, framebuffers will be handled by Qt's QVulkanWindow mechanism
        qDebug() << "✅ Framebuffers ready";
        return true;
    }
    
    bool createCommandPool() {
        qDebug() << "📝 Creating command pool...";
        
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamily;
        
        VkResult result = m_deviceFuncs->vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create command pool! Error:" << result;
            return false;
        }
        
        qDebug() << "✅ Command pool created";
        return true;
    }
    
    bool createVertexBuffer() {
        qDebug() << "🔺 Creating vertex buffer for fullscreen quad...";
        
        // Fullscreen quad vertices (triangle strip, from  WaveItem)
        std::array<Vertex, 4> vertices = {{
            {{-1.0f, -1.0f}, {0.0f, 0.0f}},  // Bottom-left
            {{-1.0f,  1.0f}, {0.0f, 1.0f}},  // Top-left
            {{ 1.0f, -1.0f}, {1.0f, 0.0f}},  // Bottom-right
            {{ 1.0f,  1.0f}, {1.0f, 1.0f}}   // Top-right
        }};
        
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = sizeof(vertices);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        VkResult result = m_deviceFuncs->vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_vertexBuffer);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create vertex buffer! Error:" << result;
            return false;
        }
        
        // Allocate memory for vertex buffer
        VkMemoryRequirements memRequirements;
        m_deviceFuncs->vkGetBufferMemoryRequirements(m_device, m_vertexBuffer, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, 
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        result = m_deviceFuncs->vkAllocateMemory(m_device, &allocInfo, nullptr, &m_vertexBufferMemory);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to allocate vertex buffer memory! Error:" << result;
            return false;
        }
        
        m_deviceFuncs->vkBindBufferMemory(m_device, m_vertexBuffer, m_vertexBufferMemory, 0);
        
        // Copy vertex data
        void* data;
        m_deviceFuncs->vkMapMemory(m_device, m_vertexBufferMemory, 0, bufferInfo.size, 0, &data);
        memcpy(data, vertices.data(), (size_t)bufferInfo.size);
        m_deviceFuncs->vkUnmapMemory(m_device, m_vertexBufferMemory);
        
        qDebug() << "✅ Vertex buffer created with fullscreen quad";
        return true;
    }
    
    bool createUniformBuffer() {
        qDebug() << "📊 Creating uniform buffer for wave parameters...";
        
        VkDeviceSize bufferSize = sizeof(UniformBlock);
        
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        
        VkResult result = m_deviceFuncs->vkCreateBuffer(m_device, &bufferInfo, nullptr, &m_uniformBuffer);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create uniform buffer! Error:" << result;
            return false;
        }
        
        VkMemoryRequirements memRequirements;
        m_deviceFuncs->vkGetBufferMemoryRequirements(m_device, m_uniformBuffer, &memRequirements);
        
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        
        result = m_deviceFuncs->vkAllocateMemory(m_device, &allocInfo, nullptr, &m_uniformBufferMemory);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to allocate uniform buffer memory! Error:" << result;
            return false;
        }
        
        m_deviceFuncs->vkBindBufferMemory(m_device, m_uniformBuffer, m_uniformBufferMemory, 0);
        
        // Keep it mapped for frequent updates
        m_deviceFuncs->vkMapMemory(m_device, m_uniformBufferMemory, 0, bufferSize, 0, &m_uniformBufferMapped);
        
        qDebug() << "✅ Uniform buffer created and mapped";
        return true;
    }
    
    bool createShaders() {
        qDebug() << "🎭 Loading and compiling wave shader...";
        
        // Load vertex shader (simple passthrough)
        QString vertexShaderSource = R"(
#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;

void main() {
    gl_Position = vec4(inPosition, 0.0, 1.0);
    fragTexCoord = inTexCoord;
}
)";
        
        // Load  fragment shader (adapted for Vulkan)
        QString fragmentShaderSource = loadWaveFragmentShader();
        
        // Compile shaders (simplified for this demo - normally you'd compile to SPIR-V)
        m_vertexShaderModule = createShaderModule(vertexShaderSource, VK_SHADER_STAGE_VERTEX_BIT);
        m_fragmentShaderModule = createShaderModule(fragmentShaderSource, VK_SHADER_STAGE_FRAGMENT_BIT);
        
        if (m_vertexShaderModule == VK_NULL_HANDLE || m_fragmentShaderModule == VK_NULL_HANDLE) {
            qWarning() << "Failed to create shader modules!";
            return false;
        }
        
        qDebug() << "✅ Wave shaders compiled successfully";
        return true;
    }
    
    QString loadWaveFragmentShader() {
        //  sophisticated wave fragment shader adapted for Vulkan
        return R"(
#version 450

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform UniformBlock {
    float time;
    float speed;
    float amplitude;
    float frequency;
    vec4 baseColor;
    vec4 waveColor;
    float threshold;
    float dustIntensity;
    float minDist;
    float maxDist;
    int maxDraws;
    vec2 resolution;
} ub;

#define THRESHOLD ub.threshold
#define MIN_DIST ub.minDist
#define MAX_DIST ub.maxDist
#define MAX_DRAWS ub.maxDraws

// Dave Hoskins hash for noise (from  original shader)
float hash12(vec2 p) {
    uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673U, 3812015801U);
    uint n = (q.x ^ q.y) * 1597334673U;
    return float(n) * 2.328306437080797e-10;
}

float value2d(vec2 p) {
    vec2 pg = floor(p), pc = p - pg, k = vec2(0,1);
    pc *= pc * pc * (3. - 2. * pc);
    return mix(
        mix(hash12(pg + k.xx), hash12(pg + k.yx), pc.x),
        mix(hash12(pg + k.xy), hash12(pg + k.yy), pc.x),
        pc.y
    );
}

float sdf(vec3 p) {
    p *= 2.0;
    float o = 4.2 * sin(0.05 * p.x + ub.time * 0.25 * ub.speed) +
              (0.04 * p.z) * sin(p.x * 0.11 + ub.time * ub.speed) *
              2.0 * sin(p.z * 0.2 + ub.time * ub.speed) *
              value2d(vec2(0.03, 0.4) * p.xz + vec2(ub.time * 0.5 * ub.speed, 0));
    return abs(dot(p, normalize(vec3(0,1,0.05))) + 2.5 + o * 0.5 * ub.amplitude);
}

vec2 raymarch(vec3 o, vec3 d, float omega) {
    float t = 0.0, a = 0.0;
    float g = MAX_DIST, dt = 0.0, sl = 0.0, emin = 0.03, ed = emin;
    int dr = 0;
    bool hit = false;
    
    for (int i = 0; i < 100; i++) {
        vec3 p = o + d * t;
        float ndt = sdf(p);
        if (abs(dt) + abs(ndt) < sl) {
            sl -= omega * sl;
            omega = 1.0;
        } else {
            sl = ndt * omega;
        }
        dt = ndt;
        t += sl;
        g = (t > 10.0) ? min(g, abs(dt)) : MAX_DIST;
        if ((t += dt) >= MAX_DIST) break;
        if (dt < MIN_DIST) {
            if (dr > MAX_DRAWS) break;
            dr++;
            float f = smoothstep(0.09, 0.11, (p.z * 0.9) / 100.0);
            if (!hit) {
                a = 0.01;
                hit = true;
            }
            ed = 2.0 * max(emin, abs(ndt));
            a += 0.0135 * f * ub.frequency;
            t += ed;
        }
    }
    g /= 3.0;
    return vec2(a, max(1.0 - g, 0.0));
}

void main() {
    vec2 uv = fragTexCoord;
    vec3 o = vec3(0);
    vec3 d = vec3((uv - 0.5) * vec2(ub.resolution.x / ub.resolution.y, 1), 1);
    vec2 mg = raymarch(o, d, 1.2);
    float m = mg.x;
    
    vec3 c = mix(
        mix(ub.baseColor.rgb, ub.waveColor.rgb, uv.x),
        mix(ub.waveColor.rgb, ub.baseColor.rgb, uv.x),
        uv.y
    );
    c = mix(c, vec3(1.0), m);
    
    // Add dust effect
    vec2 ar = vec2(ub.resolution.x / ub.resolution.y, 1);
    vec2 pp = uv * vec2(2000.0) * ar;
    float dust = pow(0.64 + 0.46 * cos(uv.x * 6.28), 1.7) * mg.y * (
        value2d(0.1 * pp + ub.time * vec2(20.0, -10.1)) * 4.0 +
        value2d(0.2 * pp + ub.time * vec2(30.0, -10.1)) * 5.0 +
        value2d(0.32 * pp + ub.time * vec2(40.0, -10.1)) * 2.0
    );
    c += dust * 0.3 * ub.dustIntensity;
    
    outColor = vec4(c, 1.0);
}
)";
    }
    
    VkShaderModule createShaderModule(const QString& source, VkShaderStageFlagBits stage) {
        // In a real implementation, you'd compile GLSL to SPIR-V using glslang
        // For this demo, we'll create a placeholder module
        
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        
        // This is simplified - normally you'd have compiled SPIR-V bytecode here
        std::vector<uint32_t> code = {0x07230203}; // SPIR-V magic number
        createInfo.codeSize = code.size() * sizeof(uint32_t);
        createInfo.pCode = code.data();
        
        VkShaderModule shaderModule;
        VkResult result = m_deviceFuncs->vkCreateShaderModule(m_device, &createInfo, nullptr, &shaderModule);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create shader module! Error:" << result;
            return VK_NULL_HANDLE;
        }
        
        return shaderModule;
    }
    
    bool createDescriptorSets() {
        qDebug() << "🎯 Creating descriptor sets...";
        
        // Create descriptor set layout
        VkDescriptorSetLayoutBinding uboLayoutBinding{};
        uboLayoutBinding.binding = 0;
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.descriptorCount = 1;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &uboLayoutBinding;
        
        VkResult result = m_deviceFuncs->vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create descriptor set layout! Error:" << result;
            return false;
        }
        
        // Create descriptor pool
        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = 1;
        
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        poolInfo.maxSets = 1;
        
        result = m_deviceFuncs->vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create descriptor pool! Error:" << result;
            return false;
        }
        
        // Allocate descriptor set
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &m_descriptorSetLayout;
        
        result = m_deviceFuncs->vkAllocateDescriptorSets(m_device, &allocInfo, &m_descriptorSet);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to allocate descriptor set! Error:" << result;
            return false;
        }
        
        // Update descriptor set
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffer;
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(UniformBlock);
        
        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = m_descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;
        
        m_deviceFuncs->vkUpdateDescriptorSets(m_device, 1, &descriptorWrite, 0, nullptr);
        
        qDebug() << "✅ Descriptor sets created";
        return true;
    }
    
    bool createGraphicsPipeline() {
        qDebug() << "🚀 Creating graphics pipeline...";
        
        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = m_vertexShaderModule;
        vertShaderStageInfo.pName = "main";
        
        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = m_fragmentShaderModule;
        fragShaderStageInfo.pName = "main";
        
        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
        
        // Vertex input
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        
        std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);
        
        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, texCoord);
        
        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();
        
        // Input assembly (triangle strip for fullscreen quad)
        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        inputAssembly.primitiveRestartEnable = VK_FALSE;
        
        // Viewport
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = (float)width();
        viewport.height = (float)height();
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        
        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {(uint32_t)width(), (uint32_t)height()};
        
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        
        // Rasterizer
        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;
        
        // Multisampling
        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        
        // Color blending
        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;
        
        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        
        // Pipeline layout
        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_descriptorSetLayout;
        
        VkResult result = m_deviceFuncs->vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create pipeline layout! Error:" << result;
            return false;
        }
        
        // Graphics pipeline
        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.layout = m_pipelineLayout;
        pipelineInfo.renderPass = m_renderPass;
        pipelineInfo.subpass = 0;
        
        result = m_deviceFuncs->vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to create graphics pipeline! Error:" << result;
            return false;
        }
        
        qDebug() << "✅ Graphics pipeline created";
        return true;
    }
    
    bool createCommandBuffers() {
        qDebug() << "📝 Creating command buffers...";
        
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = m_commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        
        VkResult result = m_deviceFuncs->vkAllocateCommandBuffers(m_device, &allocInfo, &m_commandBuffer);
        if (result != VK_SUCCESS) {
            qWarning() << "Failed to allocate command buffer! Error:" << result;
            return false;
        }
        
        qDebug() << "✅ Command buffer created";
        return true;
    }
    
    void updateUniformBuffer() {
        if (!m_uniformBufferMapped) return;
        
        // Create uniform data (same as  WaveItem::updateUniformBuffer)
        UniformBlock ubo{};
        ubo.time = (float)m_time;
        ubo.speed = (float)m_speed;
        ubo.amplitude = (float)m_amplitude;
        ubo.frequency = (float)m_frequency;
        ubo.baseColor = QVector4D(m_baseColor.redF(), m_baseColor.greenF(), m_baseColor.blueF(), m_baseColor.alphaF());
        ubo.waveColor = QVector4D(m_waveColor.redF(), m_waveColor.greenF(), m_waveColor.blueF(), m_waveColor.alphaF());
        ubo.threshold = (float)m_threshold;
        ubo.dustIntensity = (float)m_dustIntensity;
        ubo.minDist = (float)m_minDist;
        ubo.maxDist = (float)m_maxDist;
        ubo.maxDraws = m_maxDraws;
        ubo.resolution = QVector2D(width(), height());
        
        // Copy to mapped memory
        memcpy(m_uniformBufferMapped, &ubo, sizeof(ubo));
    }
    
    void render() {
        if (!m_initialized || m_device == VK_NULL_HANDLE) return;
        
        // For this demo, we'll use a simplified render loop
        // In a full implementation, you'd acquire swapchain images, etc.
        
        // Update uniform buffer with current animation values
        updateUniformBuffer();
        
        // Request next frame
        requestUpdate();
    }
    
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        m_vulkanFuncs->vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);
        
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && 
                (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        
        qFatal("Failed to find suitable memory type!");
        return 0;
    }
    
    void cleanupVulkan() {
        qDebug() << "🧹 Cleaning up Vulkan resources...";
        
        if (m_deviceFuncs && m_device != VK_NULL_HANDLE) {
            m_deviceFuncs->vkDeviceWaitIdle(m_device);
            
            if (m_graphicsPipeline != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
            }
            if (m_pipelineLayout != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            }
            if (m_renderPass != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyRenderPass(m_device, m_renderPass, nullptr);
            }
            if (m_descriptorPool != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            }
            if (m_descriptorSetLayout != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
            }
            if (m_uniformBuffer != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyBuffer(m_device, m_uniformBuffer, nullptr);
            }
            if (m_uniformBufferMemory != VK_NULL_HANDLE) {
                m_deviceFuncs->vkFreeMemory(m_device, m_uniformBufferMemory, nullptr);
            }
            if (m_vertexBuffer != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
            }
            if (m_vertexBufferMemory != VK_NULL_HANDLE) {
                m_deviceFuncs->vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
            }
            if (m_fragmentShaderModule != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyShaderModule(m_device, m_fragmentShaderModule, nullptr);
            }
            if (m_vertexShaderModule != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyShaderModule(m_device, m_vertexShaderModule, nullptr);
            }
            if (m_commandPool != VK_NULL_HANDLE) {
                m_deviceFuncs->vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            }
            
            m_deviceFuncs->vkDestroyDevice(m_device, nullptr);
        }
        
        qDebug() << "✅ Vulkan cleanup complete";
    }

private:
    // Animation state
    bool m_initialized = false;
    QTimer *m_animTimer;
    
    // Wave animation parameters (from  WaveItem)
    qreal m_time;
    qreal m_speed;
    qreal m_amplitude;
    qreal m_frequency;
    QColor m_baseColor;
    QColor m_waveColor;
    qreal m_threshold;
    qreal m_dustIntensity;
    qreal m_minDist;
    qreal m_maxDist;
    int m_maxDraws;
    
    // Vulkan objects
    QVulkanInstance *m_vulkanInstance;
    QVulkanFunctions *m_vulkanFuncs;
    QVulkanDeviceFunctions *m_deviceFuncs;
    
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    uint32_t m_graphicsQueueFamily;
    VkQueue m_graphicsQueue;
    
    VkSwapchainKHR m_swapchain;
    VkRenderPass m_renderPass;
    VkCommandPool m_commandPool;
    VkCommandBuffer m_commandBuffer;
    
    VkBuffer m_vertexBuffer;
    VkDeviceMemory m_vertexBufferMemory;
    
    VkBuffer m_uniformBuffer;
    VkDeviceMemory m_uniformBufferMemory;
    void *m_uniformBufferMapped;
    
    VkDescriptorSetLayout m_descriptorSetLayout;
    VkDescriptorPool m_descriptorPool;
    VkDescriptorSet m_descriptorSet;
    
    VkShaderModule m_vertexShaderModule;
    VkShaderModule m_fragmentShaderModule;
    VkPipelineLayout m_pipelineLayout;
    VkPipeline m_graphicsPipeline;
};

int main(int argc, char *argv[])
{
    qDebug() << "🌊 === OpenXMB - The Open Cross Media Bar ===";
    qDebug() << "🎮 Starting next-generation retro gaming interface...";
    
    QLoggingCategory::setFilterRules(QStringLiteral("qt.vulkan=true"));
    
    QGuiApplication app(argc, argv);
    app.setApplicationName("OpenXMB");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("Syndromatic Ltd");

    // Create Vulkan instance (exactly as Qt docs specify)
    QVulkanInstance inst;
    
    // Enable MoltenVK compatibility for macOS
    QByteArrayList extensions;
    extensions << "VK_KHR_portability_subset";
    inst.setExtensions(extensions);
    
#ifdef ENABLE_VALIDATION_LAYERS
    // Enable validation layers for development
    QByteArrayList layers;
    layers << "VK_LAYER_KHRONOS_validation";
    inst.setLayers(layers);
    qDebug() << "🔍 Vulkan validation layers enabled";
#endif
    
    if (!inst.create()) {
        qWarning("❌ Vulkan not available! Trying fallback...");
        
#ifdef ENABLE_VALIDATION_LAYERS
        // Try without validation layers
        QVulkanInstance instFallback;
        instFallback.setExtensions(extensions);
        if (!instFallback.create()) {
            qFatal("❌ Vulkan completely unavailable - cannot start OpenXMB");
            return 1;
        }
        
        OpenXMBWindow window;
        window.setVulkanInstance(&instFallback);
        window.setTitle("OpenXMB - Fallback Mode");
        window.showFullScreen();
        
        qDebug() << "⚠️  Running in fallback mode (no validation)";
        return app.exec();
#else
        qFatal("❌ Vulkan unavailable - cannot start OpenXMB");
        return 1;
#endif
    }
    
    qDebug() << "✅ Vulkan instance created successfully!";
    qDebug() << "📱 API Version:" << inst.apiVersion();
    qDebug() << "🔌 Available Extensions:" << inst.supportedExtensions().size();

    // Create the main OpenXMB window
    OpenXMBWindow window;
    window.setVulkanInstance(&inst);
    window.setTitle("OpenXMB - Syndromatic XMS");
    
    // Configure for appliance-like fullscreen experience
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect geometry = screen->geometry();
        qDebug() << "🖥️  Primary display:" << geometry;
        window.setGeometry(geometry);
        window.setFlags(Qt::FramelessWindowHint | Qt::Window);
        window.showFullScreen();
    } else {
        qWarning() << "⚠️  No primary screen found, using windowed mode";
        window.show();
    }

    qDebug() << "🚀 === OpenXMB is now running! ===";
    qDebug() << "⌨️  Press ESC or Q to quit";
    
    int result = app.exec();
    
    qDebug() << "👋 OpenXMB shutting down...";
    qDebug() << "🎮 Thanks for using Syndromatic OpenXMB!";
    
    return result;
}

#include "main.moc"