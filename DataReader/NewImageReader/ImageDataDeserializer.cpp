#include "stdafx.h"
#include "ImageDataDeserializer.h"
#include "ImageConfigHelper.h"
#include <opencv2/opencv.hpp>
#include "CPUSparseMatrix.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    template<class TElement>
    class TypedLabelGenerator : public ImageDataDeserializer::LabelGenerator
    {
    public:
        TypedLabelGenerator()
            : m_labelSampleLayout(std::make_shared<ImageLayout>(std::vector<size_t> { 1 }))
        {
        }

        virtual void ReadLabelDataFor(SequenceData& data, size_t classId) override
        {
            data.numberOfSamples = 1;
            data.layouts.resize(1);
            data.layouts[0] = m_labelSampleLayout;

            if (data.data == nullptr)
            {
                data.data = new char*[sizeof(size_t) + sizeof(TElement)];
            }

            *reinterpret_cast<size_t*>(data.data) = classId;
            *reinterpret_cast<TElement*>(reinterpret_cast<char*>(data.data) + sizeof(size_t)) = 1;
        }

    private:
        TensorShapePtr m_labelSampleLayout;
    };

    ImageDataDeserializer::ImageDataDeserializer(const ConfigParameters& config)
    {
        auto configHelper = ImageConfigHelper(config);
        m_streams = configHelper.GetStreams();
        assert(m_streams.size() == 2);
        const auto& label = m_streams[configHelper.GetLabelStreamId()];
        const auto& feature = m_streams[configHelper.GetFeatureStreamId()];

        label->storageType = StorageType::st_sparse_csc;
        feature->storageType = StorageType::st_dense;

        m_featureElementType = feature->elementType;
        size_t labelDimension = label->sampleLayout->GetHeight();

        if (label->elementType == ElementType::et_float)
        {
            m_labelGenerator = std::make_shared<TypedLabelGenerator<float>>();
        }
        else if (label->elementType == ElementType::et_double)
        {
            m_labelGenerator = std::make_shared<TypedLabelGenerator<double>>();
        }
        else
        {
            RuntimeError("Unsupported label element type %ull.", label->elementType);
        }

        CreateSequenceDescriptions(configHelper.GetMapPath(), labelDimension);
    }

    void ImageDataDeserializer::CreateSequenceDescriptions(std::string mapPath, size_t labelDimension)
    {
        UNREFERENCED_PARAMETER(labelDimension);

        std::ifstream mapFile(mapPath);
        if (!mapFile)
        {
            RuntimeError("Could not open %s for reading.", mapPath.c_str());
        }

        std::string line{ "" };

        ImageSequenceDescription description;
        description.numberOfSamples = 1;
        description.isValid = true;
        for (size_t cline = 0; std::getline(mapFile, line); cline++)
        {
            std::stringstream ss{ line };
            std::string imgPath;
            std::string clsId;
            if (!std::getline(ss, imgPath, '\t') || !std::getline(ss, clsId, '\t'))
            {
                RuntimeError("Invalid map file format, must contain 2 tab-delimited columns: %s, line: %d.", mapPath.c_str(), cline);
            }

            description.id = cline;
            description.chunkId = cline;
            description.path = imgPath;
            description.classId = std::stoi(clsId);
            assert(description.classId < labelDimension);
            m_imageSequences.push_back(description);
        }

        for (const auto& sequence : m_imageSequences)
        {
            m_sequences.push_back(&sequence);
        }
    }

    std::vector<StreamDescriptionPtr> ImageDataDeserializer::GetStreams() const
    {
        return m_streams;
    }

    void ImageDataDeserializer::SetEpochConfiguration(const EpochConfiguration& /* config */)
    {
    }

    const Timeline& ImageDataDeserializer::GetSequenceDescriptions() const
    {
        return m_sequences;
    }

    std::vector<std::vector<SequenceData>> ImageDataDeserializer::GetSequencesById(const std::vector<size_t> & ids)
    {
        assert(0 < ids.size());

        std::vector<std::vector<SequenceData>> result;

        m_currentImages.resize(ids.size());
        m_labels.resize(ids.size());
        result.resize(ids.size());

#pragma omp parallel for ordered schedule(dynamic)
        for (int i = 0; i < ids.size(); ++i)
        {
            assert(ids[i] < m_imageSequences.size());
            const auto& imageSequence = m_imageSequences[ids[i]];

            // Construct image
            m_currentImages[i] = std::move(cv::imread(imageSequence.path, cv::IMREAD_COLOR));
            cv::Mat& cvImage = m_currentImages[i];
            assert(cvImage.isContinuous());

            // Convert element type.
            // TODO in original image reader, this conversion happened later. Should we support all native CV element types to be able to match this behavior?
            int dataType = m_featureElementType == ElementType::et_float ? CV_32F : CV_64F;
            if (cvImage.type() != CV_MAKETYPE(dataType, cvImage.channels()))
            {
                cvImage.convertTo(cvImage, dataType);
            }

            SequenceData image;
            image.data = cvImage.ptr();
            image.layouts.push_back(std::make_shared<ImageLayout>(ImageLayoutWHC(cvImage.cols, cvImage.rows, cvImage.channels())));
            image.numberOfSamples = 1;
            assert(imageSequence.numberOfSamples == image.numberOfSamples);

            // Construct label
            m_labelGenerator->ReadLabelDataFor(m_labels[i], imageSequence.classId);

            result[i] = std::move(std::vector<SequenceData> { image, m_labels[i] });
        }

        return result;
    }

    // TODO RequireChunk: re-ahead here (in cooperation with randomizer requesting images)
    bool ImageDataDeserializer::RequireChunk(size_t /* chunkIndex */)
    {
        return true;
    }

    void ImageDataDeserializer::ReleaseChunk(size_t /* chunkIndex */)
    {
    }
}}}
