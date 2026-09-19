#include "graphics/presentation/window/metalFx.h"

#include "graphics/host_gpu/renderer/render.h"

#include <TargetConditionals.h>

#if TARGET_OS_IPHONE && __has_include(<MetalFX/MetalFX.h>)

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <vulkan/vulkan_metal.h>

#include <algorithm>
#include <atomic>
#include <cstdio>

namespace Libs::Graphics::MetalFx {

namespace {

std::atomic<bool> g_spatial {false};
std::atomic<bool> g_temporal {false};
std::atomic<bool> g_frame_interpolation {false};

MTLPixelFormat MtlFormat(vk::Format format)
{
	switch (format) {
		case vk::Format::eB8G8R8A8Unorm: return MTLPixelFormatBGRA8Unorm;
		case vk::Format::eR8G8B8A8Unorm: return MTLPixelFormatRGBA8Unorm;
		case vk::Format::eR16G16B16A16Sfloat: return MTLPixelFormatRGBA16Float;
		default: return MTLPixelFormatInvalid;
	}
}

bool ClearDepth(id<MTLCommandBuffer> cb, id<MTLTexture> depth)
{
	if (cb == nil || depth == nil) {
		return false;
	}
	MTLRenderPassDescriptor* pd            = [MTLRenderPassDescriptor renderPassDescriptor];
	pd.depthAttachment.texture             = depth;
	pd.depthAttachment.loadAction          = MTLLoadActionClear;
	pd.depthAttachment.storeAction         = MTLStoreActionStore;
	pd.depthAttachment.clearDepth          = 1.0;
	id<MTLRenderCommandEncoder> enc        = [cb renderCommandEncoderWithDescriptor:pd];
	if (enc == nil) {
		return false;
	}
	[enc endEncoding];
	return true;
}

bool ClearMotion(id<MTLCommandBuffer> cb, id<MTLTexture> motion)
{
	if (cb == nil || motion == nil) {
		return false;
	}
	MTLRenderPassDescriptor* pd            = [MTLRenderPassDescriptor renderPassDescriptor];
	pd.colorAttachments[0].texture         = motion;
	pd.colorAttachments[0].loadAction      = MTLLoadActionClear;
	pd.colorAttachments[0].storeAction     = MTLStoreActionStore;
	pd.colorAttachments[0].clearColor      = MTLClearColorMake(0, 0, 0, 0);
	id<MTLRenderCommandEncoder> enc        = [cb renderCommandEncoderWithDescriptor:pd];
	if (enc == nil) {
		return false;
	}
	[enc endEncoding];
	return true;
}

struct SharedTexture {
	id<MTLTexture> tex;
	VulkanImage    image;
	vk::DeviceMemory memory;
};

struct Chain {
	vk::Device         device;
	vk::PhysicalDevice physical_device;
	id<MTLDevice>       mtl_device;
	id<MTLCommandQueue> mtl_queue;
	bool ready         = false;
	bool export_failed = false;
	bool format_warned = false;

	SharedTexture input;
	SharedTexture real;
	SharedTexture mid;

	id<MTLTexture> prev;
	NSUInteger     prev_width  = 0;
	NSUInteger     prev_height = 0;

	id<MTLFXTemporalScaler> temporal;
	id<MTLTexture>          temporal_depth;
	id<MTLTexture>          temporal_motion;
	id<MTLTexture>          temporal_exposure;
	NSUInteger              temporal_in_width   = 0;
	NSUInteger              temporal_in_height  = 0;
	NSUInteger              temporal_out_width  = 0;
	NSUInteger              temporal_out_height = 0;

	id<MTLFXSpatialScaler> spatial;
	NSUInteger             spatial_in_width   = 0;
	NSUInteger             spatial_in_height  = 0;
	NSUInteger             spatial_out_width  = 0;
	NSUInteger             spatial_out_height = 0;

	id             fi;
	id<MTLTexture> fi_depth;
	id<MTLTexture> fi_motion;
	NSUInteger     fi_width  = 0;
	NSUInteger     fi_height = 0;
	bool           fi_reset  = false;

	bool Export(GraphicContext& graphics)
	{
		if (ready) {
			return true;
		}
		if (export_failed || graphics.device == nullptr || graphics.queue == nullptr) {
			return false;
		}
		device          = graphics.device;
		physical_device = graphics.physical_device;
		auto export_objects = reinterpret_cast<PFN_vkExportMetalObjectsEXT>(
		    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT"));
		if (export_objects == nullptr) {
			export_failed = true;
			std::printf("Haumea:MetalFX:Warning: vkExportMetalObjectsEXT missing\n");
			return false;
		}
		VkExportMetalDeviceInfoEXT device_info {VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
		VkExportMetalCommandQueueInfoEXT queue_info {
		    VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT};
		queue_info.queue  = graphics.queue;
		device_info.pNext = &queue_info;
		VkExportMetalObjectsInfoEXT info {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT};
		info.pNext = &device_info;
		export_objects(device, &info);
		mtl_device = device_info.mtlDevice;
		mtl_queue  = queue_info.mtlCommandQueue;
		if (mtl_device == nil || mtl_queue == nil) {
			export_failed = true;
			std::printf("Haumea:MetalFX:Warning: Metal object export failed\n");
			return false;
		}
		ready = true;
		std::printf("Haumea:MetalFX:Info: chain ready device=%s\n", mtl_device.name.UTF8String);
		return true;
	}

	void DestroyTexture(SharedTexture& texture)
	{
		if (texture.image.image != nullptr) {
			device.destroyImage(texture.image.image);
			texture.image.image = nullptr;
		}
		if (texture.memory != nullptr) {
			device.freeMemory(texture.memory);
			texture.memory = nullptr;
		}
		texture.tex                = nil;
		texture.image.format       = vk::Format::eUndefined;
		texture.image.extent       = {1, 1, 1};
		texture.image.state.layout = vk::ImageLayout::eUndefined;
	}

	bool EnsureTexture(SharedTexture& texture, uint32_t width, uint32_t height, vk::Format format)
	{
		if (texture.tex != nil && texture.image.extent.width == width &&
		    texture.image.extent.height == height && texture.image.format == format) {
			return true;
		}
		DestroyTexture(texture);
		const MTLPixelFormat mtl_format = MtlFormat(format);
		if (mtl_format == MTLPixelFormatInvalid || width < 1 || height < 1) {
			return false;
		}
		MTLTextureDescriptor* td =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_format
		                                                       width:width
		                                                      height:height
		                                                   mipmapped:NO];
		td.storageMode = MTLStorageModePrivate;
		td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
		texture.tex = [mtl_device newTextureWithDescriptor:td];
		if (texture.tex == nil) {
			return false;
		}

		VkImportMetalTextureInfoEXT import_info {VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT};
		import_info.plane      = VK_IMAGE_ASPECT_PLANE_0_BIT;
		import_info.mtlTexture = texture.tex;
		vk::ImageCreateInfo image_ci {};
		image_ci.sType         = vk::StructureType::eImageCreateInfo;
		image_ci.pNext         = &import_info;
		image_ci.imageType     = vk::ImageType::e2D;
		image_ci.format        = format;
		image_ci.extent        = vk::Extent3D {width, height, 1};
		image_ci.mipLevels     = 1;
		image_ci.arrayLayers   = 1;
		image_ci.samples       = vk::SampleCountFlagBits::e1;
		image_ci.tiling        = vk::ImageTiling::eOptimal;
		image_ci.usage         = vk::ImageUsageFlagBits::eTransferSrc |
		                 vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled;
		image_ci.initialLayout = vk::ImageLayout::eUndefined;
		vk::Image image        = nullptr;
		if (device.createImage(&image_ci, nullptr, &image) != vk::Result::eSuccess) {
			DestroyTexture(texture);
			return false;
		}
		texture.image.image = image;

		const auto requirements = device.getImageMemoryRequirements(image);
		if (requirements.size > 0 && requirements.memoryTypeBits != 0) {
			const auto memory_properties = physical_device.getMemoryProperties();
			uint32_t   type              = UINT32_MAX;
			for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++) {
				if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
				    (memory_properties.memoryTypes[i].propertyFlags &
				     vk::MemoryPropertyFlagBits::eDeviceLocal)) {
					type = i;
					break;
				}
			}
			vk::MemoryAllocateInfo alloc_info {};
			alloc_info.sType           = vk::StructureType::eMemoryAllocateInfo;
			alloc_info.allocationSize  = requirements.size;
			alloc_info.memoryTypeIndex = type == UINT32_MAX ? 0 : type;
			vk::DeviceMemory memory    = nullptr;
			if (device.allocateMemory(&alloc_info, nullptr, &memory) != vk::Result::eSuccess) {
				DestroyTexture(texture);
				return false;
			}
			texture.memory = memory;
			if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) {
				DestroyTexture(texture);
				return false;
			}
		}

		texture.image.format       = format;
		texture.image.image_type   = vk::ImageType::e2D;
		texture.image.extent       = vk::Extent3D {width, height, 1};
		texture.image.layers       = 1;
		texture.image.mip_levels   = 1;
		texture.image.samples      = 1;
		texture.image.usage        = image_ci.usage;
		texture.image.state.layout = vk::ImageLayout::eTransferSrcOptimal;
		return true;
	}

	void DropTemporal()
	{
		temporal          = nil;
		temporal_depth    = nil;
		temporal_motion   = nil;
		temporal_exposure = nil;
		temporal_in_width = temporal_in_height = 0;
		temporal_out_width = temporal_out_height = 0;
	}

	void DropSpatial()
	{
		spatial          = nil;
		spatial_in_width = spatial_in_height = 0;
		spatial_out_width = spatial_out_height = 0;
	}

	void DropFi()
	{
		fi        = nil;
		fi_depth  = nil;
		fi_motion = nil;
		fi_width = fi_height = 0;
		fi_reset             = false;
	}

	void ResetChain()
	{
		DropTemporal();
		DropSpatial();
		DropFi();
		if (device != nullptr) {
			DestroyTexture(input);
			DestroyTexture(real);
			DestroyTexture(mid);
		}
		prev       = nil;
		prev_width = prev_height = 0;
	}

	id<MTLTexture> Upscale(id<MTLCommandBuffer> cb, id<MTLTexture> source, NSUInteger out_width,
	                       NSUInteger out_height, MTLPixelFormat format, bool want_temporal)
	    API_AVAILABLE(ios(16.0))
	{
		const NSUInteger in_width  = source.width;
		const NSUInteger in_height = source.height;
		if (want_temporal && [MTLFXTemporalScalerDescriptor supportsDevice:mtl_device]) {
			bool       rebuilt = false;
			const bool need = temporal == nil || temporal_in_width != in_width ||
			                  temporal_in_height != in_height || temporal_out_width != out_width ||
			                  temporal_out_height != out_height;
			if (need) {
				DropTemporal();
				MTLFXTemporalScalerDescriptor* td = [[MTLFXTemporalScalerDescriptor alloc] init];
				td.colorTextureFormat  = format;
				td.depthTextureFormat  = MTLPixelFormatDepth32Float;
				td.motionTextureFormat = MTLPixelFormatRG16Float;
				td.outputTextureFormat = format;
				td.inputWidth          = in_width;
				td.inputHeight         = in_height;
				td.outputWidth         = out_width;
				td.outputHeight        = out_height;
				td.autoExposureEnabled = NO;
				temporal               = [td newTemporalScalerWithDevice:mtl_device];
				if (temporal != nil) {
					rebuilt = true;
					MTLTextureDescriptor* dep = [MTLTextureDescriptor
					    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
					                                 width:in_width
					                                height:in_height
					                             mipmapped:NO];
					dep.storageMode = MTLStorageModePrivate;
					dep.usage       = temporal.depthTextureUsage | MTLTextureUsageRenderTarget;
					temporal_depth  = [mtl_device newTextureWithDescriptor:dep];

					MTLTextureDescriptor* mot = [MTLTextureDescriptor
					    texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
					                                 width:in_width
					                                height:in_height
					                             mipmapped:NO];
					mot.storageMode = MTLStorageModePrivate;
					mot.usage       = temporal.motionTextureUsage | MTLTextureUsageRenderTarget;
					temporal_motion = [mtl_device newTextureWithDescriptor:mot];

					MTLTextureDescriptor* exd = [MTLTextureDescriptor
					    texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
					                                 width:1
					                                height:1
					                             mipmapped:NO];
					exd.storageMode   = MTLStorageModeShared;
					exd.usage         = MTLTextureUsageShaderRead;
					temporal_exposure = [mtl_device newTextureWithDescriptor:exd];
					if (temporal_exposure != nil) {
						static const uint16_t one = 0x3c00;
						[temporal_exposure replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
						                     mipmapLevel:0
						                       withBytes:&one
						                     bytesPerRow:2];
					}
					if (temporal_depth == nil || temporal_motion == nil ||
					    temporal_exposure == nil) {
						DropTemporal();
					} else {
						temporal_in_width   = in_width;
						temporal_in_height  = in_height;
						temporal_out_width  = out_width;
						temporal_out_height = out_height;
					}
				}
			}
			if (temporal != nil && ClearDepth(cb, temporal_depth) &&
			    ClearMotion(cb, temporal_motion)) {
				temporal.colorTexture       = source;
				temporal.depthTexture       = temporal_depth;
				temporal.motionTexture      = temporal_motion;
				temporal.outputTexture      = real.tex;
				temporal.inputContentWidth  = in_width;
				temporal.inputContentHeight = in_height;
				temporal.exposureTexture    = temporal_exposure;
				temporal.reactiveMaskTexture = nil;
				temporal.preExposure        = 1.f;
				temporal.jitterOffsetX      = 0.f;
				temporal.jitterOffsetY      = 0.f;
				temporal.motionVectorScaleX = static_cast<float>(in_width);
				temporal.motionVectorScaleY = static_cast<float>(in_height);
				temporal.reset              = rebuilt ? YES : NO;
				temporal.depthReversed      = NO;
				temporal.fence              = nil;
				[temporal encodeToCommandBuffer:cb];
				return real.tex;
			}
		}

		if (![MTLFXSpatialScalerDescriptor supportsDevice:mtl_device]) {
			return source;
		}
		const bool need = spatial == nil || spatial_in_width != in_width ||
		                  spatial_in_height != in_height || spatial_out_width != out_width ||
		                  spatial_out_height != out_height;
		if (need) {
			DropSpatial();
			MTLFXSpatialScalerDescriptor* sd = [[MTLFXSpatialScalerDescriptor alloc] init];
			sd.colorTextureFormat  = format;
			sd.outputTextureFormat = format;
			sd.inputWidth          = in_width;
			sd.inputHeight         = in_height;
			sd.outputWidth         = out_width;
			sd.outputHeight        = out_height;
			sd.colorProcessingMode = format == MTLPixelFormatRGBA16Float
			                             ? MTLFXSpatialScalerColorProcessingModeHDR
			                             : MTLFXSpatialScalerColorProcessingModePerceptual;
			spatial                = [sd newSpatialScalerWithDevice:mtl_device];
			if (spatial == nil) {
				return source;
			}
			spatial_in_width   = in_width;
			spatial_in_height  = in_height;
			spatial_out_width  = out_width;
			spatial_out_height = out_height;
		}
		spatial.colorTexture       = source;
		spatial.inputContentWidth  = in_width;
		spatial.inputContentHeight = in_height;
		spatial.outputTexture      = real.tex;
		[spatial encodeToCommandBuffer:cb];
		return real.tex;
	}

	id<MTLTexture> Interpolate(id<MTLCommandBuffer> cb, id<MTLTexture> current,
	                           MTLPixelFormat format, double target_fps)
	{
		if (@available(iOS 26.0, *)) {
			const NSUInteger width  = current.width;
			const NSUInteger height = current.height;
			if (prev == nil || prev_width != width || prev_height != height) {
				return nil;
			}
			if (![MTLFXFrameInterpolatorDescriptor supportsDevice:mtl_device]) {
				return nil;
			}
			if (fi == nil || fi_width != width || fi_height != height || fi_depth == nil ||
			    fi_motion == nil) {
				DropFi();
				MTLFXFrameInterpolatorDescriptor* fd =
				    [[MTLFXFrameInterpolatorDescriptor alloc] init];
				fd.colorTextureFormat  = format;
				fd.outputTextureFormat = format;
				fd.depthTextureFormat  = MTLPixelFormatDepth32Float;
				fd.motionTextureFormat = MTLPixelFormatRG16Float;
				fd.inputWidth          = width;
				fd.inputHeight         = height;
				fd.outputWidth         = width;
				fd.outputHeight        = height;
				id interpolator = [fd newFrameInterpolatorWithDevice:mtl_device];
				if (interpolator == nil) {
					return nil;
				}
				MTLTextureDescriptor* dd = [MTLTextureDescriptor
				    texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
				                                 width:width
				                                height:height
				                             mipmapped:NO];
				dd.storageMode = MTLStorageModePrivate;
				dd.usage       = ((id<MTLFXFrameInterpolator>)interpolator).depthTextureUsage |
				           MTLTextureUsageRenderTarget;
				id<MTLTexture> depth = [mtl_device newTextureWithDescriptor:dd];
				MTLTextureDescriptor* md = [MTLTextureDescriptor
				    texture2DDescriptorWithPixelFormat:MTLPixelFormatRG16Float
				                                 width:width
				                                height:height
				                             mipmapped:NO];
				md.storageMode = MTLStorageModePrivate;
				md.usage       = ((id<MTLFXFrameInterpolator>)interpolator).motionTextureUsage |
				           MTLTextureUsageRenderTarget;
				id<MTLTexture> motion = [mtl_device newTextureWithDescriptor:md];
				if (depth == nil || motion == nil) {
					return nil;
				}
				fi        = interpolator;
				fi_depth  = depth;
				fi_motion = motion;
				fi_width  = width;
				fi_height = height;
				fi_reset  = true;
			}
			if (!ClearDepth(cb, fi_depth) || !ClearMotion(cb, fi_motion)) {
				return nil;
			}
			id<MTLFXFrameInterpolator> interpolator = fi;
			interpolator.colorTexture       = current;
			interpolator.prevColorTexture   = prev;
			interpolator.depthTexture       = fi_depth;
			interpolator.motionTexture      = fi_motion;
			interpolator.uiTexture          = nil;
			interpolator.outputTexture      = mid.tex;
			interpolator.deltaTime = static_cast<float>(1.0 / std::max(1.0, target_fps));
			interpolator.shouldResetHistory = fi_reset ? YES : NO;
			fi_reset                        = false;
			interpolator.jitterOffsetX      = 0.f;
			interpolator.jitterOffsetY      = 0.f;
			interpolator.motionVectorScaleX = static_cast<float>(width);
			interpolator.motionVectorScaleY = static_cast<float>(height);
			interpolator.fieldOfView        = 60.f;
			interpolator.aspectRatio =
			    static_cast<float>(width) / static_cast<float>(std::max<NSUInteger>(1, height));
			interpolator.nearPlane          = 0.1f;
			interpolator.farPlane           = 100.f;
			interpolator.depthReversed      = NO;
			interpolator.uiTextureComposited = NO;
			[interpolator encodeToCommandBuffer:cb];
			return mid.tex;
		}
		return nil;
	}

	void CopyToPrev(id<MTLCommandBuffer> cb, id<MTLTexture> tex)
	{
		const NSUInteger width  = tex.width;
		const NSUInteger height = tex.height;
		if (prev == nil || prev_width != width || prev_height != height) {
			MTLTextureDescriptor* pd =
			    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat
			                                                       width:width
			                                                      height:height
			                                                   mipmapped:NO];
			pd.storageMode = MTLStorageModePrivate;
			pd.usage       = MTLTextureUsageShaderRead;
			prev           = [mtl_device newTextureWithDescriptor:pd];
			prev_width     = prev != nil ? width : 0;
			prev_height    = prev != nil ? height : 0;
			fi_reset       = true;
		}
		if (prev == nil) {
			return;
		}
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		if (blit == nil) {
			return;
		}
		[blit copyFromTexture:tex
		          sourceSlice:0
		          sourceLevel:0
		         sourceOrigin:MTLOriginMake(0, 0, 0)
		           sourceSize:MTLSizeMake(width, height, 1)
		            toTexture:prev
		     destinationSlice:0
		     destinationLevel:0
		    destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];
	}
};

Chain g_chain;

}

void SetSettings(bool spatial, bool temporal, bool frame_interpolation)
{
	g_spatial.store(spatial, std::memory_order_relaxed);
	g_temporal.store(temporal, std::memory_order_relaxed);
	g_frame_interpolation.store(frame_interpolation, std::memory_order_relaxed);
}

bool Wanted()
{
	return g_spatial.load(std::memory_order_relaxed) ||
	       g_temporal.load(std::memory_order_relaxed) ||
	       g_frame_interpolation.load(std::memory_order_relaxed);
}

Frames Run(GraphicContext& graphics, CommandBuffer& command, VulkanImage& frame,
           vk::Extent2D output, bool allow_interpolation, double target_fps)
{
	Frames result {};
	if (@available(iOS 16.0, *)) {
		const uint32_t frame_width  = frame.extent.width;
		const uint32_t frame_height = frame.extent.height;
		if (frame_width < 1 || frame_height < 1 || output.width < 1 || output.height < 1) {
			return result;
		}
		const MTLPixelFormat format = MtlFormat(frame.format);
		if (format == MTLPixelFormatInvalid) {
			if (!g_chain.format_warned) {
				g_chain.format_warned = true;
				std::printf("Haumea:MetalFX:Warning: unsupported frame format=%d\n",
				            static_cast<int>(frame.format));
			}
			return result;
		}
		if (!g_chain.Export(graphics)) {
			return result;
		}
		if (!g_chain.EnsureTexture(g_chain.input, frame_width, frame_height, frame.format)) {
			return result;
		}

		auto vk_command = command.Handle();
		command.Begin();
		vk::ImageMemoryBarrier to_transfer {};
		to_transfer.sType                       = vk::StructureType::eImageMemoryBarrier;
		to_transfer.dstAccessMask               = vk::AccessFlagBits::eTransferWrite;
		to_transfer.oldLayout                   = vk::ImageLayout::eUndefined;
		to_transfer.newLayout                   = vk::ImageLayout::eTransferDstOptimal;
		to_transfer.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
		to_transfer.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
		to_transfer.image                       = g_chain.input.image.image;
		to_transfer.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		to_transfer.subresourceRange.levelCount = 1;
		to_transfer.subresourceRange.layerCount = 1;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
		                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
		                           nullptr, 0, nullptr, 1, &to_transfer);
		vk::ImageCopy copy {};
		copy.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		copy.srcSubresource.layerCount = 1;
		copy.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
		copy.dstSubresource.layerCount = 1;
		copy.extent                    = vk::Extent3D {frame_width, frame_height, 1};
		vk_command.copyImage(frame.image, vk::ImageLayout::eTransferSrcOptimal,
		                     g_chain.input.image.image, vk::ImageLayout::eTransferDstOptimal, 1,
		                     &copy);
		vk::ImageMemoryBarrier back_to_source {};
		back_to_source.sType                       = vk::StructureType::eImageMemoryBarrier;
		back_to_source.srcAccessMask               = vk::AccessFlagBits::eTransferWrite;
		back_to_source.oldLayout                   = vk::ImageLayout::eTransferDstOptimal;
		back_to_source.newLayout                   = vk::ImageLayout::eTransferSrcOptimal;
		back_to_source.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
		back_to_source.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
		back_to_source.image                       = g_chain.input.image.image;
		back_to_source.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
		back_to_source.subresourceRange.levelCount = 1;
		back_to_source.subresourceRange.layerCount = 1;
		vk_command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                           vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags {}, 0,
		                           nullptr, 0, nullptr, 1, &back_to_source);
		command.End();
		command.Execute();

		@autoreleasepool {
			id<MTLCommandBuffer> cb = [g_chain.mtl_queue commandBuffer];
			if (cb == nil) {
				return result;
			}

			id<MTLTexture> current = g_chain.input.tex;
			const bool want_upscale =
			    (g_spatial.load(std::memory_order_relaxed) ||
			     g_temporal.load(std::memory_order_relaxed)) &&
			    (output.width > frame_width || output.height > frame_height);
			if (want_upscale) {
				if (!g_chain.EnsureTexture(g_chain.real, output.width, output.height,
				                           frame.format)) {
					return result;
				}
				current = g_chain.Upscale(cb, current, output.width, output.height, format,
				                          g_temporal.load(std::memory_order_relaxed));
			}

			id<MTLTexture> interpolated = nil;
			if (g_frame_interpolation.load(std::memory_order_relaxed)) {
				if (allow_interpolation &&
				    g_chain.EnsureTexture(g_chain.mid, static_cast<uint32_t>(current.width),
				                          static_cast<uint32_t>(current.height), frame.format)) {
					interpolated = g_chain.Interpolate(cb, current, format, target_fps);
				}
				g_chain.CopyToPrev(cb, current);
			}

			[cb commit];

			if (current != g_chain.input.tex) {
				result.real = &g_chain.real.image;
			}
			if (interpolated != nil) {
				result.mid = &g_chain.mid.image;
			}
		}
	}
	return result;
}

void Reset()
{
	g_chain.ResetChain();
}

}

#else

namespace Libs::Graphics::MetalFx {

void SetSettings(bool spatial, bool temporal, bool frame_interpolation)
{
	(void)spatial;
	(void)temporal;
	(void)frame_interpolation;
}

bool Wanted()
{
	return false;
}

Frames Run(GraphicContext& graphics, CommandBuffer& command, VulkanImage& frame,
           vk::Extent2D output, bool allow_interpolation, double target_fps)
{
	(void)graphics;
	(void)command;
	(void)frame;
	(void)output;
	(void)allow_interpolation;
	(void)target_fps;
	return {};
}

void Reset() {}

}

#endif

extern "C" void haumea_set_metal_fx(bool spatial, bool temporal, bool frame_interpolation)
{
	Libs::Graphics::MetalFx::SetSettings(spatial, temporal, frame_interpolation);
}
