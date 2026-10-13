// Vulkan 1.0 entry points used by the backend. Resolve through SDL so the
// executable can still run its software/OpenGL renderers without a Vulkan
// loader installed. This is application dispatch code, not a bundled loader.
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#define NBLOOD_VULKAN_GLOBAL_COMMANDS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties)

#define NBLOOD_VULKAN_INSTANCE_COMMANDS(X) \
    X(vkDestroyInstance) \
    X(vkDestroyDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkCreateDevice) \
    X(vkDestroySurfaceKHR) \
    X(vkEnumeratePhysicalDevices) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)

#define NBLOOD_VULKAN_DEVICE_COMMANDS(X) \
    X(vkAcquireNextImageKHR) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkBindImageMemory) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBlitImage) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdDraw) \
    X(vkCmdEndRenderPass) \
    X(vkCmdExecuteCommands) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdSetDepthBias) \
    X(vkCmdSetLineWidth) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetViewport) \
    X(vkCreateBuffer) \
    X(vkCreateCommandPool) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateFence) \
    X(vkCreateFramebuffer) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImage) \
    X(vkCreateImageView) \
    X(vkCreatePipelineCache) \
    X(vkCreatePipelineLayout) \
    X(vkCreateRenderPass) \
    X(vkCreateSampler) \
    X(vkCreateSemaphore) \
    X(vkCreateShaderModule) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroyBuffer) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyFence) \
    X(vkDestroyFramebuffer) \
    X(vkDestroyImage) \
    X(vkDestroyImageView) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineCache) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroyRenderPass) \
    X(vkDestroySampler) \
    X(vkDestroySemaphore) \
    X(vkDestroyShaderModule) \
    X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkFreeCommandBuffers) \
    X(vkFreeMemory) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetDeviceQueue) \
    X(vkGetFenceStatus) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetSwapchainImagesKHR) \
    X(vkMapMemory) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkQueueWaitIdle) \
    X(vkResetCommandPool) \
    X(vkResetDescriptorPool) \
    X(vkResetFences) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences)
