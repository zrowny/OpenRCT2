/*****************************************************************************
 * Copyright (c) 2014-2020 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#include "ImageTable.h"

#include "../Context.h"
#include "../OpenRCT2.h"
#include "../PlatformEnvironment.h"
#include "../core/File.h"
#include "../core/FileScanner.h"
#include "../core/IStream.hpp"
#include "../core/Json.hpp"
#include "../core/Path.hpp"
#include "../core/String.hpp"
#include "../drawing/ImageImporter.h"
#include "../sprites.h"
#include "Object.h"
#include "ObjectFactory.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

using namespace OpenRCT2;
using namespace OpenRCT2::Drawing;

struct ImageTable::RequiredImage
{
    rct_g1_element g1{};
    std::unique_ptr<RequiredImage> next_zoom;

    bool HasData() const
    {
        return g1.offset != nullptr;
    }

    RequiredImage() = default;
    RequiredImage(const RequiredImage&) = delete;

    RequiredImage(const rct_g1_element& orig)
    {
        auto length = g1_calculate_data_size(&orig);
        g1 = orig;
        g1.offset = new uint8_t[length];
        std::memcpy(g1.offset, orig.offset, length);
        g1.flags &= ~G1_FLAG_HAS_ZOOM_SPRITE;
    }

    RequiredImage(uint32_t idx, std::function<const rct_g1_element*(uint32_t)> getter)
    {
        auto orig = getter(idx);
        if (orig != nullptr)
        {
            auto length = g1_calculate_data_size(orig);
            g1 = *orig;
            g1.offset = new uint8_t[length];
            std::memcpy(g1.offset, orig->offset, length);
            if ((g1.flags & G1_FLAG_HAS_ZOOM_SPRITE) && g1.zoomed_offset != 0)
            {
                // Fetch image for next zoom level
                next_zoom = std::make_unique<RequiredImage>(static_cast<uint32_t>(idx - g1.zoomed_offset), getter);
                if (!next_zoom->HasData())
                {
                    next_zoom = nullptr;
                    g1.flags &= ~G1_FLAG_HAS_ZOOM_SPRITE;
                }
            }
        }
    }

    ~RequiredImage()
    {
        delete[] g1.offset;
    }
};

std::vector<std::unique_ptr<ImageTable::RequiredImage>> ImageTable::ParseImages(IReadObjectContext* context, std::string s)
{
    std::vector<std::unique_ptr<RequiredImage>> result;
    if (s.empty())
    {
        result.push_back(std::make_unique<RequiredImage>());
    }
    else if (String::StartsWith(s, "$CSG"))
    {
        if (is_csg_loaded())
        {
            auto range = ParseRange(s.substr(4));
            if (!range.empty())
            {
                for (auto i : range)
                {
                    result.push_back(std::make_unique<RequiredImage>(
                        static_cast<uint32_t>(SPR_CSG_BEGIN + i),
                        [](uint32_t idx) -> const rct_g1_element* { return gfx_get_g1_element(idx); }));
                }
            }
        }
    }
    else if (String::StartsWith(s, "$G1"))
    {
        auto range = ParseRange(s.substr(3));
        if (!range.empty())
        {
            for (auto i : range)
            {
                result.push_back(std::make_unique<RequiredImage>(
                    static_cast<uint32_t>(i), [](uint32_t idx) -> const rct_g1_element* { return gfx_get_g1_element(idx); }));
            }
        }
    }
    else if (String::StartsWith(s, "$RCT2:OBJDATA/"))
    {
        auto name = s.substr(14);
        auto rangeStart = name.find('[');
        if (rangeStart != std::string::npos)
        {
            auto rangeString = name.substr(rangeStart);
            auto range = ParseRange(name.substr(rangeStart));
            name = name.substr(0, rangeStart);
            result = LoadObjectImages(context, name, range);
        }
    }
    else
    {
        try
        {
            auto imageData = context->GetData(s);
            auto image = Imaging::ReadFromBuffer(imageData, IMAGE_FORMAT::PNG_32);

            ImageImporter importer;
            auto importResult = importer.Import(image, 0, 0, ImageImporter::IMPORT_FLAGS::RLE);

            result.push_back(std::make_unique<RequiredImage>(importResult.Element));
        }
        catch (const std::exception& e)
        {
            auto msg = String::StdFormat("Unable to load image '%s': %s", s.c_str(), e.what());
            context->LogWarning(ObjectError::BadImageTable, msg.c_str());
            result.push_back(std::make_unique<RequiredImage>());
        }
    }
    return result;
}

std::vector<std::unique_ptr<ImageTable::RequiredImage>> ImageTable::ParseImages(IReadObjectContext* context, json_t& el)
{
    Guard::Assert(el.is_object(), "ImageTable::ParseImages expects parameter el to be object");

    auto path = Json::GetString(el["path"]);
    auto x = Json::GetNumber<int16_t>(el["x"]);
    auto y = Json::GetNumber<int16_t>(el["y"]);
    auto raw = Json::GetString(el["format"]) == "raw";
    auto keepPalette = Json::GetBoolean(el["keepPalette"]);

    std::vector<std::unique_ptr<RequiredImage>> result;
    try
    {
        auto flags = ImageImporter::IMPORT_FLAGS::NONE;
        if (!raw)
        {
            flags = static_cast<ImageImporter::IMPORT_FLAGS>(flags | ImageImporter::IMPORT_FLAGS::RLE);
        }
        IMAGE_FORMAT format = IMAGE_FORMAT::PNG_32;
        if (keepPalette)
        {
            flags = static_cast<ImageImporter::IMPORT_FLAGS>(flags | ImageImporter::IMPORT_FLAGS::KEEP_PALETTE);
            format = IMAGE_FORMAT::PNG;
        }
        auto imageData = context->GetData(path);
        auto image = Imaging::ReadFromBuffer(imageData, format);

        ImageImporter importer;
        auto importResult = importer.Import(image, 0, 0, flags);
        auto g1Element = importResult.Element;
        g1Element.x_offset = x;
        g1Element.y_offset = y;
        result.push_back(std::make_unique<RequiredImage>(g1Element));
    }
    catch (const std::exception& e)
    {
        auto msg = String::StdFormat("Unable to load image '%s': %s", path.c_str(), e.what());
        context->LogWarning(ObjectError::BadImageTable, msg.c_str());
        result.push_back(std::make_unique<RequiredImage>());
    }
    return result;
}

std::vector<std::unique_ptr<ImageTable::RequiredImage>> ImageTable::LoadObjectImages(
    IReadObjectContext* context, const std::string& name, const std::vector<int32_t>& range)
{
    std::vector<std::unique_ptr<RequiredImage>> result;
    auto objectPath = FindLegacyObject(name);
    auto obj = ObjectFactory::CreateObjectFromLegacyFile(context->GetObjectRepository(), objectPath.c_str());
    if (obj != nullptr)
    {
        auto& imgTable = static_cast<const Object*>(obj.get())->GetImageTable();
        auto numImages = static_cast<int32_t>(imgTable.GetCount());
        auto images = imgTable.GetImages();
        size_t placeHoldersAdded = 0;
        for (auto i : range)
        {
            if (i >= 0 && i < numImages)
            {
                result.push_back(std::make_unique<RequiredImage>(
                    static_cast<uint32_t>(i), [images](uint32_t idx) -> const rct_g1_element* { return &images[idx]; }));
            }
            else
            {
                result.push_back(std::make_unique<RequiredImage>());
                placeHoldersAdded++;
            }
        }

        // Log place holder information
        if (placeHoldersAdded > 0)
        {
            std::string msg = "PAdding " + std::to_string(placeHoldersAdded) + " placeholders";
            context->LogWarning(ObjectError::InvalidProperty, msg.c_str());
        }
    }
    else
    {
        std::string msg = "Unable to open '" + objectPath + "'";
        context->LogWarning(ObjectError::InvalidProperty, msg.c_str());
        for (size_t i = 0; i < range.size(); i++)
        {
            result.push_back(std::make_unique<RequiredImage>());
        }
    }
    return result;
}

std::vector<int32_t> ImageTable::ParseRange(std::string s)
{
    // Currently only supports [###] or [###..###]
    std::vector<int32_t> result = {};
    if (s.length() >= 3 && s[0] == '[' && s[s.length() - 1] == ']')
    {
        s = s.substr(1, s.length() - 2);
        auto parts = String::Split(s, "..");
        if (parts.size() == 1)
        {
            result.push_back(std::stoi(parts[0]));
        }
        else
        {
            auto left = std::stoi(parts[0]);
            auto right = std::stoi(parts[1]);
            if (left <= right)
            {
                for (auto i = left; i <= right; i++)
                {
                    result.push_back(i);
                }
            }
            else
            {
                for (auto i = right; i >= left; i--)
                {
                    result.push_back(i);
                }
            }
        }
    }
    return result;
}

std::string ImageTable::FindLegacyObject(const std::string& name)
{
    const auto env = GetContext()->GetPlatformEnvironment();
    auto objectsPath = env->GetDirectoryPath(DIRBASE::RCT2, DIRID::OBJECT);
    auto objectPath = Path::Combine(objectsPath, name);
    if (!File::Exists(objectPath))
    {
        // Search recursively for any file with the target name (case insensitive)
        auto filter = Path::Combine(objectsPath, "*.dat");
        auto scanner = std::unique_ptr<IFileScanner>(Path::ScanDirectory(filter, true));
        while (scanner->Next())
        {
            auto currentName = Path::GetFileName(scanner->GetPathRelative());
            if (String::Equals(currentName, name, true))
            {
                objectPath = scanner->GetPath();
                break;
            }
        }
    }
    return objectPath;
}

ImageTable::~ImageTable()
{
    if (_data == nullptr)
    {
        for (auto& entry : _entries)
        {
            delete[] entry.offset;
        }
    }
}

void ImageTable::Read(IReadObjectContext* context, OpenRCT2::IStream* stream)
{
    if (gOpenRCT2NoGraphics)
    {
        return;
    }

    try
    {
        uint32_t numImages = stream->ReadValue<uint32_t>();
        uint32_t imageDataSize = stream->ReadValue<uint32_t>();

        uint64_t headerTableSize = numImages * 16;
        uint64_t remainingBytes = stream->GetLength() - stream->GetPosition() - headerTableSize;
        if (remainingBytes > imageDataSize)
        {
            context->LogWarning(ObjectError::BadImageTable, "Image table size longer than expected.");
            imageDataSize = static_cast<uint32_t>(remainingBytes);
        }

        auto dataSize = static_cast<size_t>(imageDataSize);
        auto data = std::make_unique<uint8_t[]>(dataSize);
        if (data == nullptr)
        {
            context->LogError(ObjectError::BadImageTable, "Image table too large.");
            throw std::runtime_error("Image table too large.");
        }

        // Read g1 element headers
        uintptr_t imageDataBase = reinterpret_cast<uintptr_t>(data.get());
        std::vector<rct_g1_element> newEntries;
        for (uint32_t i = 0; i < numImages; i++)
        {
            rct_g1_element g1Element{};

            uintptr_t imageDataOffset = static_cast<uintptr_t>(stream->ReadValue<uint32_t>());
            g1Element.offset = reinterpret_cast<uint8_t*>(imageDataBase + imageDataOffset);

            g1Element.width = stream->ReadValue<int16_t>();
            g1Element.height = stream->ReadValue<int16_t>();
            g1Element.x_offset = stream->ReadValue<int16_t>();
            g1Element.y_offset = stream->ReadValue<int16_t>();
            g1Element.flags = stream->ReadValue<uint16_t>();
            g1Element.zoomed_offset = stream->ReadValue<uint16_t>();

            newEntries.push_back(std::move(g1Element));
        }

        // Read g1 element data
        size_t readBytes = static_cast<size_t>(stream->TryRead(data.get(), dataSize));

        // If data is shorter than expected (some custom objects are unfortunately like that)
        size_t unreadBytes = dataSize - readBytes;
        if (unreadBytes > 0)
        {
            std::fill_n(data.get() + readBytes, unreadBytes, 0);
            context->LogWarning(ObjectError::BadImageTable, "Image table size shorter than expected.");
        }

        _data = std::move(data);
        _entries.insert(_entries.end(), newEntries.begin(), newEntries.end());
    }
    catch (const std::exception&)
    {
        context->LogError(ObjectError::BadImageTable, "Bad image table.");
        throw;
    }
}

void ImageTable::ReadJson(IReadObjectContext* context, json_t& root)
{
    Guard::Assert(root.is_object(), "ImageTable::ReadJson expects parameter root to be object");

    if (context->ShouldLoadImages())
    {
        // First gather all the required images from inspecting the JSON
        std::vector<std::unique_ptr<RequiredImage>> allImages;
        auto jsonImages = root["images"];

        for (auto& jsonImage : jsonImages)
        {
            if (jsonImage.is_string())
            {
                auto strImage = jsonImage.get<std::string>();
                auto images = ParseImages(context, strImage);
                allImages.insert(
                    allImages.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
            }
            else if (jsonImage.is_object())
            {
                auto images = ParseImages(context, jsonImage);
                allImages.insert(
                    allImages.end(), std::make_move_iterator(images.begin()), std::make_move_iterator(images.end()));
            }
        }

        // Now add all the images to the image table
        auto imagesStartIndex = GetCount();
        for (const auto& img : allImages)
        {
            const auto& g1 = img->g1;
            AddImage(&g1);
        }

        // Add all the zoom images at the very end of the image table.
        // This way it should not affect the offsets used within the object logic.
        for (size_t j = 0; j < allImages.size(); j++)
        {
            const auto tableIndex = imagesStartIndex + j;
            const auto* img = allImages[j].get();
            if (img->next_zoom != nullptr)
            {
                img = img->next_zoom.get();

                // Set old image zoom offset to zoom image which we are about to add
                auto g1a = const_cast<rct_g1_element*>(&GetImages()[tableIndex]);
                g1a->zoomed_offset = static_cast<int32_t>(tableIndex) - static_cast<int32_t>(GetCount());

                while (img != nullptr)
                {
                    auto g1b = img->g1;
                    if (img->next_zoom != nullptr)
                    {
                        g1b.zoomed_offset = -1;
                    }
                    AddImage(&g1b);
                    img = img->next_zoom.get();
                }
            }
        }
    }
}

void ImageTable::AddImage(const rct_g1_element* g1)
{
    rct_g1_element newg1 = *g1;
    auto length = g1_calculate_data_size(g1);
    if (length == 0)
    {
        newg1.offset = nullptr;
    }
    else
    {
        newg1.offset = new uint8_t[length];
        std::copy_n(g1->offset, length, newg1.offset);
    }
    _entries.push_back(std::move(newg1));
}
