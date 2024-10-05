#include <cstring>

#include "d3d11_device.h"
#include "d3d11_initializer.h"

namespace dxvk {

  D3D11Initializer::D3D11Initializer(
          D3D11Device*                pParent)
  : m_parent(pParent),
    m_device(pParent->GetDXVKDevice()),
    m_context(m_device->createContext(DxvkContextType::Supplementary)),
    m_stagingBuffer(m_device, StagingBufferSize) {
    m_context->beginRecording(
      m_device->createCommandList());
  }

  
  D3D11Initializer::~D3D11Initializer() {

  }


  void D3D11Initializer::Flush() {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    if (m_transferCommands != 0)
      FlushInternal();
  }

  void D3D11Initializer::InitBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    if (!(pBuffer->Desc()->MiscFlags & D3D11_RESOURCE_MISC_TILED)) {
      VkMemoryPropertyFlags memFlags = pBuffer->GetBuffer()->memFlags();

      (memFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        ? InitHostVisibleBuffer(pBuffer, pInitialData)
        : InitDeviceLocalBuffer(pBuffer, pInitialData);
    }
  }
  

  void D3D11Initializer::InitTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    if (pTexture->Desc()->MiscFlags & D3D11_RESOURCE_MISC_TILED)
      InitTiledTexture(pTexture);
    else if (pTexture->GetMapMode() == D3D11_COMMON_TEXTURE_MAP_MODE_DIRECT)
      InitHostVisibleTexture(pTexture, pInitialData);
    else
      InitDeviceLocalTexture(pTexture, pInitialData);

    SyncSharedTexture(pTexture);
  }


  void D3D11Initializer::InitUavCounter(
          D3D11UnorderedAccessView*   pUav) {
    auto counterView = pUav->GetCounterView();

    if (counterView == nullptr)
      return;

    DxvkBufferSlice counterSlice(counterView);

    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_transferCommands += 1;

    const uint32_t zero = 0;
    m_context->updateBuffer(
      counterSlice.buffer(),
      counterSlice.offset(),
      sizeof(zero), &zero);

    FlushImplicit();
  }


  void D3D11Initializer::InitDeviceLocalBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    Rc<DxvkBuffer> buffer = pBuffer->GetBuffer();

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      auto stagingSlice = m_stagingBuffer.alloc(buffer->info().size);
      std::memcpy(stagingSlice.mapPtr(0), pInitialData->pSysMem, stagingSlice.length());

      m_transferMemory += buffer->info().size;
      m_transferCommands += 1;
      
      m_context->uploadBuffer(buffer,
        stagingSlice.buffer(),
        stagingSlice.offset());
    } else {
      m_transferCommands += 1;

      m_context->initBuffer(buffer);
    }

    FlushImplicit();
  }


  void D3D11Initializer::InitHostVisibleBuffer(
          D3D11Buffer*                pBuffer,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    // If the buffer is mapped, we can write data directly
    // to the mapped memory region instead of doing it on
    // the GPU. Same goes for zero-initialization.
    DxvkBufferSlice bufferSlice = pBuffer->GetBufferSlice();

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      std::memcpy(
        bufferSlice.mapPtr(0),
        pInitialData->pSysMem,
        bufferSlice.length());
    } else {
      std::memset(
        bufferSlice.mapPtr(0), 0,
        bufferSlice.length());
    }
  }


  void D3D11Initializer::InitDeviceLocalTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    
    // Image migt be null if this is a staging resource
    Rc<DxvkImage> image = pTexture->GetImage();

    auto mapMode = pTexture->GetMapMode();
    auto desc = pTexture->Desc();

    VkFormat packedFormat = m_parent->LookupPackedFormat(desc->Format, pTexture->GetFormatMode()).Format;
    auto formatInfo = lookupFormatInfo(packedFormat);

    if (pInitialData != nullptr && pInitialData->pSysMem != nullptr) {
      // Compute data size for all subresources and allocate staging buffer memory
      DxvkBufferSlice stagingSlice;

      if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_STAGING) {
        VkDeviceSize dataSize = 0u;

        for (uint32_t mip = 0; mip < image->info().mipLevels; mip++) {
          dataSize += image->info().numLayers * util::computeImageDataSize(
            packedFormat, image->mipLevelExtent(mip), formatInfo->aspectMask);
        }

        stagingSlice = m_stagingBuffer.alloc(dataSize);
      }

      // Copy initial data for each subresource into the staging buffer,
      // as well as the mapped per-subresource buffers if available.
      VkDeviceSize dataOffset = 0u;

      for (uint32_t mip = 0; mip < desc->MipLevels; mip++) {
        for (uint32_t layer = 0; layer < desc->ArraySize; layer++) {
          uint32_t index = D3D11CalcSubresource(mip, layer, desc->MipLevels);
          VkExtent3D mipLevelExtent = pTexture->MipLevelExtent(mip);

          if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_STAGING) {
            VkDeviceSize mipSizePerLayer = util::computeImageDataSize(
              packedFormat, image->mipLevelExtent(mip), formatInfo->aspectMask);

            m_transferCommands += 1;
            m_transferMemory += mipSizePerLayer;

            util::packImageData(stagingSlice.mapPtr(dataOffset),
              pInitialData[index].pSysMem, pInitialData[index].SysMemPitch, pInitialData[index].SysMemSlicePitch,
              0, 0, pTexture->GetVkImageType(), mipLevelExtent, 1, formatInfo, formatInfo->aspectMask);

            dataOffset += mipSizePerLayer;
          }

          if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_NONE) {
            util::packImageData(pTexture->GetMappedBuffer(index)->mapPtr(0),
              pInitialData[index].pSysMem, pInitialData[index].SysMemPitch, pInitialData[index].SysMemSlicePitch,
              0, 0, pTexture->GetVkImageType(), mipLevelExtent, 1, formatInfo, formatInfo->aspectMask);
          }
        }
      }

      // Upload all subresources of the image in one go
      if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_STAGING)
        m_context->uploadImage(image, stagingSlice.buffer(), stagingSlice.offset(), packedFormat);
    } else {
      if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_STAGING) {
        m_transferCommands += 1;
        
        // While the Microsoft docs state that resource contents are
        // undefined if no initial data is provided, some applications
        // expect a resource to be pre-cleared.
        VkImageSubresourceRange subresources;
        subresources.aspectMask     = formatInfo->aspectMask;
        subresources.baseMipLevel   = 0;
        subresources.levelCount     = desc->MipLevels;
        subresources.baseArrayLayer = 0;
        subresources.layerCount     = desc->ArraySize;

        m_context->initImage(image, subresources, VK_IMAGE_LAYOUT_UNDEFINED);
      }

      if (mapMode != D3D11_COMMON_TEXTURE_MAP_MODE_NONE) {
        for (uint32_t i = 0; i < pTexture->CountSubresources(); i++) {
          auto buffer = pTexture->GetMappedBuffer(i);
          std::memset(buffer->mapPtr(0), 0, buffer->info().size);
        }
      }
    }

    FlushImplicit();
  }


  void D3D11Initializer::InitHostVisibleTexture(
          D3D11CommonTexture*         pTexture,
    const D3D11_SUBRESOURCE_DATA*     pInitialData) {
    Rc<DxvkImage> image = pTexture->GetImage();

    for (uint32_t layer = 0; layer < image->info().numLayers; layer++) {
      for (uint32_t level = 0; level < image->info().mipLevels; level++) {
        VkImageSubresource subresource;
        subresource.aspectMask = image->formatInfo()->aspectMask;
        subresource.mipLevel   = level;
        subresource.arrayLayer = layer;

        VkExtent3D blockCount = util::computeBlockCount(
          image->mipLevelExtent(level),
          image->formatInfo()->blockSize);

        VkSubresourceLayout layout = image->querySubresourceLayout(subresource);

        auto initialData = pInitialData
          ? &pInitialData[D3D11CalcSubresource(level, layer, image->info().mipLevels)]
          : nullptr;

        for (uint32_t z = 0; z < blockCount.depth; z++) {
          for (uint32_t y = 0; y < blockCount.height; y++) {
            auto size = blockCount.width * image->formatInfo()->elementSize;
            auto dst = image->mapPtr(layout.offset + y * layout.rowPitch + z * layout.depthPitch);

            if (initialData) {
              auto src = reinterpret_cast<const char*>(initialData->pSysMem)
                       + y * initialData->SysMemPitch
                       + z * initialData->SysMemSlicePitch;
              std::memcpy(dst, src, size);
            } else {
              std::memset(dst, 0, size);
            }
          }
        }
      }
    }

    // Initialize the image on the GPU
    std::lock_guard<dxvk::mutex> lock(m_mutex);

    VkImageSubresourceRange subresources = image->getAvailableSubresources();
    
    m_context->initImage(image, subresources, VK_IMAGE_LAYOUT_PREINITIALIZED);

    m_transferCommands += 1;
    FlushImplicit();
  }


  void D3D11Initializer::InitTiledTexture(
          D3D11CommonTexture*         pTexture) {
    m_context->initSparseImage(pTexture->GetImage());

    m_transferCommands += 1;
    FlushImplicit();
  }


  void D3D11Initializer::FlushImplicit() {
    if (m_transferCommands > MaxTransferCommands
     || m_transferMemory   > MaxTransferMemory)
      FlushInternal();
  }


  void D3D11Initializer::FlushInternal() {
    m_context->flushCommandList(nullptr);
    
    m_transferCommands = 0;
    m_transferMemory   = 0;

    m_stagingBuffer.reset();
  }


  void D3D11Initializer::SyncSharedTexture(D3D11CommonTexture* pResource) {
    if (!(pResource->Desc()->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE)))
      return;

    // Ensure that initialization commands are submitted and waited on before
    // returning control to the application in order to avoid race conditions
    // in case the texture is used immediately on a secondary device.
    auto mapMode = pResource->GetMapMode();

    if (mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_NONE
     || mapMode == D3D11_COMMON_TEXTURE_MAP_MODE_BUFFER) {
      FlushInternal();

      m_device->waitForResource(pResource->GetImage(), DxvkAccess::Write);
    }

    // If a keyed mutex is used, initialize that to the correct state as well.
    Com<IDXGIKeyedMutex> keyedMutex;

    if (SUCCEEDED(pResource->GetInterface()->QueryInterface(
        __uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&keyedMutex)))) {
      keyedMutex->AcquireSync(0, 0);
      keyedMutex->ReleaseSync(0);
    }
  }

}
