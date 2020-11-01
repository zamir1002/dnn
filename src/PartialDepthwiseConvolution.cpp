#include "Model.h"

using namespace dnnl;

namespace dnn
{
	PartialDepthwiseConvolution::PartialDepthwiseConvolution(const dnn::Device& device, const dnnl::memory::format_tag format, const std::string& name, const std::vector<Layer*>& inputs, const size_t group, const size_t groups, const size_t kernelH, const size_t kernelW, const size_t strideH, const size_t strideW, const size_t dilationH, const size_t dilationW, const size_t padH, const size_t padW, const size_t multiplier, const bool hasBias) :
		Layer(device, format, name, LayerTypes::PartialDepthwiseConvolution, multiplier * inputs[0]->C / groups * kernelH * kernelW, multiplier * inputs[0]->C / groups, multiplier * inputs[0]->C / groups, inputs[0]->D, ((((inputs[0]->H - (1 + (kernelH - 1) * dilationH)) + (padH * 2)) / strideH) + 1), ((((inputs[0]->W - (1 + (kernelW - 1) * dilationW)) + (padW * 2)) / strideW) + 1), 0, padH, padW, inputs, hasBias),
		Group(group),
		Groups(groups),
		Multiplier(multiplier),
		KernelH(kernelH),
		KernelW(kernelW),
		StrideH(strideH),
		StrideW(strideW),
		DilationH(dilationH),
		DilationW(dilationW),
		DilationKernelH(1 + (kernelH - 1) * dilationH),
		DilationKernelW(1 + (kernelW - 1) * dilationW),
		Strides(dnnl::memory::dims({ dnnl::memory::dim(strideH), dnnl::memory::dim(strideW) })),
		Dilates(dnnl::memory::dims({ dnnl::memory::dim(dilationH - 1), dnnl::memory::dim(dilationW - 1) })),
		Padding(dnnl::memory::dims({ dnnl::memory::dim(padH), dnnl::memory::dim(padW) })),
		reorderFwdSrc(false),
		reorderBwdSrc(false),
		reorderBwdDiffSrc(false),
		reorderBwdWeights(false),
		reorderBwdDiffWeights(false)
	{
		assert(Inputs.size() == 1);
		assert(Multiplier > 0);
		assert(Group > 0 && Groups > 0);
		assert(Group <= Groups);

		PersistWeightsMemDesc = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(InputLayer->C/Groups), dnnl::memory::dim(Multiplier), dnnl::memory::dim(1), dnnl::memory::dim(KernelH), dnnl::memory::dim(KernelW) }), dnnl::memory::data_type::f32, dnnl::memory::format_tag::goihw));
		WeightsMemDesc		  = std::make_unique<dnnl::memory::desc>(dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(InputLayer->C/Groups), dnnl::memory::dim(Multiplier), dnnl::memory::dim(1), dnnl::memory::dim(KernelH), dnnl::memory::dim(KernelW) }), dnnl::memory::data_type::f32, dnnl::memory::format_tag::goihw));
	}

	std::string PartialDepthwiseConvolution::GetDescription() const
	{
		std::string description = GetDescriptionHeader();

		description.append(nwl + " Groups:" + tab + std::to_string(Groups));
		description.append(nwl + " Group:" + dtab + std::to_string(Group));
		if (Multiplier > 1)
			description.append(nwl + " Multiplier:" + tab + std::to_string(Multiplier));
		if (DilationH == 1 && DilationW == 1)
			description.append(nwl + " Kernel:" + tab + std::to_string(KernelH) + "x" + std::to_string(KernelW));
		else
		{
			description.append(nwl + " Dilates:" + tab + std::to_string(DilationH) + "x" + std::to_string(DilationW));
			description.append(nwl + " Kernel:" + tab + std::to_string(DilationKernelH) + "x" + std::to_string(DilationKernelW));
		}
		if (StrideH * StrideW > 1)
			description.append(nwl + " Stride:" + tab + std::to_string(StrideH) + "x" + std::to_string(StrideW));
		if (HasPadding)
			description.append(nwl + " Padding:" + tab + std::to_string(PadH) + "x" + std::to_string(PadW));
		
		description.append(GetWeightsDescription(true));

		description.append(nwl + " Connections:" + tab + std::to_string(C * (H / StrideH) * (W / StrideW) * (HasBias ? KernelH * KernelW + 1 : KernelH * KernelW)));
		
		return description;
	}

	size_t PartialDepthwiseConvolution::FanIn() const
	{
		return Groups * KernelH * KernelW;
	}

	size_t PartialDepthwiseConvolution::FanOut() const
	{
		return Multiplier * KernelH * KernelW / StrideH * StrideW;
	}

	void PartialDepthwiseConvolution::InitializeDescriptors(const size_t batchSize)
	{
		if (InputLayer->PaddedC % Groups != 0)
			throw std::exception("input not splittable in PartialDepthwiseConvolution");

		std::vector<dnnl::memory::desc> memDesc = std::vector<dnnl::memory::desc>({
			dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(InputLayer->C/Groups), dnnl::memory::dim(InputLayer->H), dnnl::memory::dim(InputLayer->W) }), dnnl::memory::data_type::f32, Format),
			dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(batchSize), dnnl::memory::dim(C), dnnl::memory::dim(H), dnnl::memory::dim(W) }), dnnl::memory::data_type::f32, Format),
			dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(C), dnnl::memory::dim(Multiplier), dnnl::memory::dim(1), dnnl::memory::dim(KernelH), dnnl::memory::dim(KernelW) }), dnnl::memory::data_type::f32, dnnl::memory::format_tag::any),
			dnnl::memory::desc(dnnl::memory::dims({ dnnl::memory::dim(C) }), dnnl::memory::data_type::f32, dnnl::memory::format_tag::any) });

		fwdDesc = std::make_unique<dnnl::convolution_forward::primitive_desc>(dnnl::convolution_forward::primitive_desc(HasBias ? 
			dnnl::convolution_forward::desc(dnnl::prop_kind::forward, dnnl::algorithm::convolution_auto, memDesc[0], memDesc[2], memDesc[3], memDesc[1], Strides, Dilates, Padding, Padding) : 
			dnnl::convolution_forward::desc(dnnl::prop_kind::forward, dnnl::algorithm::convolution_auto, memDesc[0], memDesc[2], memDesc[1], Strides, Dilates, Padding, Padding), 
			Device.first));

		bwdWeightsDesc = std::make_unique<dnnl::convolution_backward_weights::primitive_desc>(dnnl::convolution_backward_weights::primitive_desc(HasBias ? 
			dnnl::convolution_backward_weights::desc(dnnl::algorithm::convolution_auto, memDesc[0], memDesc[2], memDesc[3], memDesc[1], Strides, Dilates, Padding, Padding) : 
			dnnl::convolution_backward_weights::desc(dnnl::algorithm::convolution_auto, memDesc[0], memDesc[2], memDesc[1], Strides, Dilates, Padding, Padding), 
			Device.first, *fwdDesc));
		
		bwdDataDesc = std::make_unique<dnnl::convolution_backward_data::primitive_desc>(dnnl::convolution_backward_data::primitive_desc(dnnl::convolution_backward_data::desc(dnnl::algorithm::convolution_auto, memDesc[0], memDesc[2], memDesc[1], Strides, Dilates, Padding, Padding), Device.first, *fwdDesc));

		if (*WeightsMemDesc != fwdDesc->weights_desc())
		{
			auto weights = FloatVector(fwdDesc->weights_desc().get_size());
			auto memWeights = dnnl::memory(*WeightsMemDesc, Device.first, Weights.data());
			auto weightsMem = dnnl::memory(fwdDesc->weights_desc(), Device.first, weights.data());

			dnnl::reorder(memWeights, weightsMem).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memWeights}, {DNNL_ARG_TO, weightsMem} });
			Device.second.wait();

			Weights = weights;
			WeightsMemDesc = std::make_unique<dnnl::memory::desc>(fwdDesc->weights_desc());
		}

		DstMemDesc = std::make_unique<dnnl::memory::desc>(fwdDesc->dst_desc());
		DiffDstMemDesc = std::make_unique<dnnl::memory::desc>(fwdDesc->dst_desc());

		partSrc = InputLayer->DstMemDesc->submemory_desc({ dnnl::memory::dim(batchSize), dnnl::memory::dim(InputLayer->C / Groups), dnnl::memory::dim(InputLayer->H), dnnl::memory::dim(InputLayer->W) }, { 0, dnnl::memory::dim((Group - 1) * C), 0, 0 });
		partDiffSrc = InputLayer->DiffDstMemDesc->submemory_desc({ dnnl::memory::dim(batchSize), dnnl::memory::dim(InputLayer->C / Groups), dnnl::memory::dim(InputLayer->H), dnnl::memory::dim(InputLayer->W) }, { 0, dnnl::memory::dim((Group - 1) * C), 0, 0 });

		reorderFwdSrc = fwdDesc->src_desc() != partSrc;
		reorderBwdSrc = bwdWeightsDesc->src_desc() != partSrc;
		reorderBwdDiffWeights = bwdWeightsDesc->diff_weights_desc() != fwdDesc->weights_desc();
		reorderBwdDiffSrc = bwdDataDesc->diff_src_desc() != partDiffSrc;
		reorderBwdWeights = bwdDataDesc->weights_desc() != *WeightsMemDesc;

#ifdef DNN_CACHE_PRIMITIVES
		fwd = std::make_unique<dnnl::convolution_forward>(dnnl::convolution_forward(*fwdDesc));
		bwdWeights = std::make_unique<dnnl::convolution_backward_weights>(dnnl::convolution_backward_weights(*bwdWeightsDesc));
		bwdData = std::make_unique<dnnl::convolution_backward_data>(dnnl::convolution_backward_data(*bwdDataDesc));
#endif
	}

	void PartialDepthwiseConvolution::ForwardProp(const size_t batchSize, const bool training)
	{
		auto memSrc = dnnl::memory(partSrc, Device.first, InputLayer->Neurons.data());
		auto srcMem = reorderFwdSrc ? dnnl::memory(fwdDesc->src_desc(), Device.first) : memSrc;
		if (reorderFwdSrc)
		{
			dnnl::reorder(memSrc, srcMem).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memSrc}, {DNNL_ARG_TO, srcMem} });
			Device.second.wait();
		}

		auto weightsMem = dnnl::memory(*WeightsMemDesc, Device.first, Weights.data());
		auto dstMem = dnnl::memory(*DstMemDesc, Device.first, Neurons.data());

#ifdef DNN_CACHE_PRIMITIVES
		HasBias ? 
			fwd->execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem} , {DNNL_ARG_WEIGHTS, weightsMem}, {DNNL_ARG_BIAS, dnnl::memory(fwdDesc->bias_desc(), Device.first, Biases.data())}, {DNNL_ARG_DST, dstMem} }) :
			fwd->execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem} , {DNNL_ARG_WEIGHTS, weightsMem}, {DNNL_ARG_DST, dstMem} });
#else
		HasBias ? dnnl::convolution_forward(*fwdDesc).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem} , {DNNL_ARG_WEIGHTS, weightsMem}, {DNNL_ARG_BIAS, dnnl::memory(fwdDesc->bias_desc(), Device.first, Biases.data())}, {DNNL_ARG_DST, dstMem} }) :
			dnnl::convolution_forward(*fwdDesc).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_SRC, srcMem} , {DNNL_ARG_WEIGHTS, weightsMem}, {DNNL_ARG_DST, dstMem} });
#endif
		Device.second.wait();

#ifndef DNN_LEAN
		if (training)
			ZeroFloatVector(NeuronsD1.data(), batchSize * PaddedCDHW);
#else
		DNN_UNREF_PAR(batchSize);
#endif // DNN_LEAN
	}

	void PartialDepthwiseConvolution::BackwardProp(const size_t batchSize)
	{
#ifdef DNN_LEAN
		ZeroGradient(batchSize);
#else
		DNN_UNREF_PAR(batchSize);
#endif // DNN_LEAN

		auto diffDstMem = dnnl::memory(*DiffDstMemDesc, Device.first, NeuronsD1.data());
		
		auto memSrc = dnnl::memory(partSrc, Device.first, InputLayer->Neurons.data());
		auto srcMem = reorderBwdSrc ? dnnl::memory(bwdWeightsDesc->src_desc(), Device.first) : memSrc;
		if (reorderBwdSrc)
		{
			dnnl::reorder(memSrc, srcMem).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memSrc}, {DNNL_ARG_TO, srcMem} });
			Device.second.wait();
		}

		auto memDiffWeights = dnnl::memory(*WeightsMemDesc, Device.first, WeightsD1.data());
		auto diffWeightsMem = reorderBwdDiffWeights ? dnnl::memory(bwdWeightsDesc->diff_weights_desc(), Device.first) : memDiffWeights;

#ifdef DNN_CACHE_PRIMITIVES
		HasBias ?
			bwdWeights->execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, {DNNL_ARG_SRC, srcMem}, {DNNL_ARG_DIFF_WEIGHTS, diffWeightsMem}, {DNNL_ARG_DIFF_BIAS, dnnl::memory(bwdWeightsDesc->diff_bias_desc(), Device.first, BiasesD1.data())} }) :
			bwdWeights->execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, {DNNL_ARG_SRC, srcMem}, {DNNL_ARG_DIFF_WEIGHTS, diffWeightsMem} });
#else
		HasBias ?
			dnnl::convolution_backward_weights(*bwdWeightsDesc).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, {DNNL_ARG_SRC, srcMem}, {DNNL_ARG_DIFF_WEIGHTS, diffWeightsMem}, {DNNL_ARG_DIFF_BIAS, dnnl::memory(bwdWeightsDesc->diff_bias_desc(), Device.first, BiasesD1.data())} }) :
			dnnl::convolution_backward_weights(*bwdWeightsDesc).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, {DNNL_ARG_SRC, srcMem}, {DNNL_ARG_DIFF_WEIGHTS, diffWeightsMem} });
#endif
		Device.second.wait();

		if (reorderBwdDiffWeights)
		{
			dnnl::reorder(diffWeightsMem, memDiffWeights).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, diffWeightsMem}, {DNNL_ARG_TO, memDiffWeights} });
			Device.second.wait();
		}

		auto memWeights = dnnl::memory(*WeightsMemDesc, Device.first, Weights.data());
		auto weightsMem = reorderBwdWeights ? dnnl::memory(bwdDataDesc->weights_desc(), Device.first) : memWeights;
		if (reorderBwdWeights)
		{
			dnnl::reorder(memWeights, weightsMem).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memWeights}, {DNNL_ARG_TO, weightsMem} });
			Device.second.wait();
		}

		auto memDiffSrc = dnnl::memory(partDiffSrc, Device.first, InputLayer->NeuronsD1.data());
		auto diffSrcMem = reorderBwdDiffSrc ? dnnl::memory(bwdDataDesc->diff_src_desc(), Device.first) : memDiffSrc;

#ifdef DNN_CACHE_PRIMITIVES
		bwdData->execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, {DNNL_ARG_WEIGHTS, weightsMem}, {DNNL_ARG_DIFF_SRC, diffSrcMem} });
#else
		dnnl::convolution_backward_data(*bwdDataDesc).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_DIFF_DST, diffDstMem}, {DNNL_ARG_WEIGHTS, weightsMem}, {DNNL_ARG_DIFF_SRC, diffSrcMem} });
#endif
		Device.second.wait();

		if (reorderBwdDiffSrc)
		{
			dnnl::reorder(diffSrcMem, memDiffSrc).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, diffSrcMem}, {DNNL_ARG_TO, memDiffSrc} });
			Device.second.wait();
		}

#ifdef DNN_LEAN
		ReleaseGradient();
#endif // DNN_LEAN
	}

	ByteVector PartialDepthwiseConvolution::GetImage(const Byte fillColor)
	{
		const auto rangeWeights = GetColorRange(WeightsMin, WeightsMax);
		const auto rangeBiases = GetColorRange(BiasesMin, BiasesMax);
		const auto border = 1ull;
		const auto pitchH = KernelH + border;
		const auto pitchW = KernelW + border;
		const auto width = C * pitchH + border;
		const auto height = pitchW + border;
		const auto biasOffset = height * width;
		auto image = ByteVector(biasOffset + width, fillColor);
		FloatVector weights;

		if (*WeightsMemDesc != *PersistWeightsMemDesc)
		{
			weights = FloatVector(WeightsMemDesc->get_size());

			auto memWeights = dnnl::memory(*WeightsMemDesc, Device.first, Weights.data());
			auto weightsMem = dnnl::memory(*PersistWeightsMemDesc, Device.first, weights.data());

			dnnl::reorder(memWeights, weightsMem).execute(Device.second, std::unordered_map<int, dnnl::memory>{ {DNNL_ARG_FROM, memWeights}, { DNNL_ARG_TO, weightsMem } });
			Device.second.wait();
		}
		else
			weights = Weights;

		for (auto c = 0ull; c < C; c++)
		{
			const auto left = c * pitchH + border;
			const auto idx = c * KernelH * KernelW;
			for (auto y = 0ull; y < KernelH; y++)
				for (auto x = 0ull; x < KernelW; x++)
					image[(y * width) + left + x] = GetColorFromRange(rangeWeights, WeightsMin, weights[idx + (y * KernelW) + x]);

			if (HasBias)
				image[left + biasOffset] = GetColorFromRange(rangeBiases, BiasesMin, Biases[c]);
		}

		return image;
	}
}