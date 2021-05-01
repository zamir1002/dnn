#pragma once
#include "Layer.h"

namespace dnn
{
	enum class Algorithms
	{
		Linear = 0,
		Nearest = 1
	};

	class Resampling final : public Layer
	{
	private:
		std::unique_ptr<dnnl::resampling_forward::primitive_desc> fwdDesc;
		std::unique_ptr<dnnl::resampling_backward::primitive_desc> bwdDesc;
		std::unique_ptr<dnnl::binary::primitive_desc> bwdAddDesc;
#ifdef DNN_CACHE_PRIMITIVES
		std::unique_ptr<dnnl::resampling_forward> fwd;
		std::unique_ptr<dnnl::resampling_backward> bwd;
		std::unique_ptr<dnnl::binary> bwdAdd;
#endif

	public:
		const Algorithms Algorithm;
		const Float FactorH;
		const Float FactorW;

		Resampling(const dnn::Device& device, const dnnl::memory::format_tag format, const std::string& name, const std::vector<Layer*>& inputs, const Algorithms algorithm, const Float factorH, const Float factorW) :
			Layer(device, format, name, LayerTypes::Resampling, 0, 0, inputs[0]->C, inputs[0]->D, static_cast<UInt>(inputs[0]->H* double(factorH)), static_cast<UInt>(inputs[0]->W* double(factorW)), 0, 0, 0, inputs),
			Algorithm(algorithm),
			FactorH(factorH),
			FactorW(factorW)
		{
			assert(Inputs.size() == 1);
		}

		std::string GetDescription() const final override
		{
			auto description = GetDescriptionHeader();

			description.append(nwl + std::string(" Scaling:") + tab + FloatToString(FactorH, 4) + std::string("x") + FloatToString(FactorW, 4));
			if (Algorithm == Algorithms::Linear)
				description.append(nwl + std::string(" Algorithm:\tlinear"));
			else
				description.append(nwl + std::string(" Algorithm:\tnearest"));
			return description;
		}

		UInt FanIn() const final override
		{
			return 1;
		}

		UInt FanOut() const final override
		{
			return 1;
		}

		void InitializeDescriptors(const UInt batchSize) final override
		{
			dnnl::algorithm algorithm;
			switch (Algorithm)
			{
			case Algorithms::Linear:
				algorithm = dnnl::algorithm::resampling_linear;
				break;
			case Algorithms::Nearest:
				algorithm = dnnl::algorithm::resampling_nearest;
				break;
			default:
				algorithm = dnnl::algorithm::resampling_linear;
			}

			const auto factor = std::vector<float>({ FactorH, FactorW });

			auto memDesc = std::vector<dnnl::memory::desc>({
				dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(InputLayer->C), dnnl::memory::dim(InputLayer->H), dnnl::memory::dim(InputLayer->W) }), dnnl::memory::data_type::f32, Format),
				dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C), dnnl::memory::dim(H), dnnl::memory::dim(W) }), dnnl::memory::data_type::f32, Format) });

			fwdDesc = std::make_unique<dnnl::resampling_forward::primitive_desc>(dnnl::resampling_forward::primitive_desc(dnnl::resampling_forward::desc(dnnl::prop_kind::forward, algorithm, factor, *InputLayer->DstMemDesc, memDesc[1]), Device.engine));
			
			DstMemDesc = std::make_unique<dnnl::memory::desc>(fwdDesc->dst_desc());
			DiffDstMemDesc = std::make_unique<dnnl::memory::desc>(fwdDesc->dst_desc());

			if (Format == dnnl::memory::format_tag::any)
				ChosenFormat = GetDataFmt(*DstMemDesc);
			else
				ChosenFormat = PlainFmt;

			bwdDesc = std::make_unique<dnnl::resampling_backward::primitive_desc>(dnnl::resampling_backward::primitive_desc(dnnl::resampling_backward::desc(algorithm, factor, memDesc[0], *DiffDstMemDesc), Device.engine, *fwdDesc));
			bwdAddDesc = std::make_unique<dnnl::binary::primitive_desc>(dnnl::binary::primitive_desc(dnnl::binary::desc(dnnl::algorithm::binary_add, *InputLayer->DiffDstMemDesc, *InputLayer->DiffDstMemDesc, *InputLayer->DiffDstMemDesc), Device.engine));

#ifdef DNN_CACHE_PRIMITIVES
			fwd = std::make_unique<dnnl::resampling_forward>(dnnl::resampling_forward(*fwdDesc));
			bwd = std::make_unique<dnnl::resampling_backward>(dnnl::resampling_backward(*bwdDesc));
			bwdAdd = std::make_unique<dnnl::binary>(dnnl::binary(*bwdAddDesc));
#endif
		}

		void ForwardProp(const UInt batchSize, const bool training) final override
		{
			auto memSrc = dnnl::memory(*InputLayer->DstMemDesc, Device.engine, InputLayer->Neurons.data());
			auto dstMem = dnnl::memory(*DstMemDesc, Device.engine, Neurons.data());

#ifdef DNN_CACHE_PRIMITIVES
			fwd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, memSrc}, { DNNL_ARG_DST, dstMem } });
#else
			dnnl::resampling_forward(*fwdDesc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, memSrc}, { DNNL_ARG_DST, dstMem } });
#endif
			Device.stream.wait();

#ifndef DNN_LEAN
			if (training)
				ZeroArray<Float>(NeuronsD1.data(), batchSize * PaddedCDHW);
#else
			DNN_UNREF_PAR(batchSize);
			DNN_UNREF_PAR(training);
#endif
		}

		void BackwardProp(const UInt batchSize) final override
		{
#ifdef DNN_LEAN
			ZeroGradient(batchSize);
#else
			DNN_UNREF_PAR(batchSize);
#endif // DNN_LEAN

			auto diffDstMem = dnnl::memory(*DiffDstMemDesc, Device.engine, NeuronsD1.data());
			auto memDiffSrc = SharesInput ? dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine) : dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data());

#ifdef DNN_CACHE_PRIMITIVES
			bwd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, { DNNL_ARG_DIFF_SRC, memDiffSrc } });
#else
			dnnl::resampling_backward(*bwdDesc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, { DNNL_ARG_DIFF_SRC, memDiffSrc } });
#endif
			Device.stream.wait();

			if (SharesInput)
			{
#ifdef DNN_CACHE_PRIMITIVES
				bwdAdd->execute(Device.stream, std::unordered_map<int, dnnl::memory>{ { DNNL_ARG_SRC_0, dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data()) }, { DNNL_ARG_SRC_1, memDiffSrc }, { DNNL_ARG_DST, dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data()) } });
#else
				dnnl::binary(*bwdAddDesc).execute(Device.stream, std::unordered_map<int, dnnl::memory>{ { DNNL_ARG_SRC_0, dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data()) }, { DNNL_ARG_SRC_1, memDiffSrc }, { DNNL_ARG_DST, dnnl::memory(*InputLayer->DiffDstMemDesc, Device.engine, InputLayer->NeuronsD1.data()) } });
#endif
				Device.stream.wait();
			}

#ifdef DNN_LEAN
			ReleaseGradient();
#endif // DNN_LEAN
		}
	};
}