#include "GpuTestHost.h"
#include "GpuCompletionProbe.h"
#include "RenderTargetProbe.h"
#include "GpuClosedListProbe.h"
#include "GpuSignatureProbe.h"
#include <d3dcompiler.h>
#include <future>
#include <thread>
#include <MinHook.h>

int main() {
    try {
        require(GpuSignatureProbe::get().install_device_creation(),"observe real test device and queues before native creation");
        Host host;auto& targets=RenderTargetProbe::get();auto& probe=*new GpuCompletionProbe;
        D3D12_INDIRECT_ARGUMENT_DESC old_argument{};old_argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC old_signature_desc{};old_signature_desc.ByteStride=sizeof(D3D12_DISPATCH_ARGUMENTS);old_signature_desc.NumArgumentDescs=1;old_signature_desc.pArgumentDescs=&old_argument;
        ComPtr<ID3D12CommandSignature> unknown_signature;check(host.device->CreateCommandSignature(&old_signature_desc,nullptr,IID_PPV_ARGS(&unknown_signature)));
        check(unknown_signature->SetPrivateData(GpuAllocationProbe::SIGNATURE_KEY,0,nullptr));
        std::optional<GpuCompletionProbe::Origin> writer_origin;bool change_origin{},fail_origin{},clear_origin{},deny_binding_coverage{};
        const auto texture=host.color(.2f,.4f,.6f,8,DXGI_FORMAT_R8G8B8A8_UNORM,4);
        const auto other=host.color(.6f,.4f,.2f,8,DXGI_FORMAT_R8G8B8A8_UNORM,4);
        targets.observe_binding=[&](uintptr_t command,std::span<const uintptr_t> resources,bool known){probe.bound(command,resources,known);};
        require(targets.install(host.device.Get()),"real native RTV hooks install");
        require(probe.install(host.device.Get(),host.queue.Get(),[&](uintptr_t handle){return targets.canonical_resource(handle);},[&] {
            (void)probe.latest(); // Re-entry proves the ownership callback holds no GPU observer lock.
            if(clear_origin) probe.clear_target();
            if(fail_origin) throw std::runtime_error("unknown source camera");
            const auto result=writer_origin;if(change_origin && writer_origin) ++writer_origin->render_id;return result;
        },[&](uintptr_t command){return !deny_binding_coverage && targets.observes_binding(command);}),"real native command lifecycle, draw, clear and queue hooks install");
        {   // No target (every play session): submissions, on any queue, go straight through; no target evidence.
            D3D12_COMMAND_QUEUE_DESC side_desc{};side_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;ComPtr<ID3D12CommandQueue> side;check(host.device->CreateCommandQueue(&side_desc,IID_PPV_ARGS(&side)));
            host.begin();host.commands->CopyResource(texture.Get(),other.Get());check(host.commands->Close());ID3D12CommandList* untargeted[]{host.commands.Get()};side->ExecuteCommandLists(1,untargeted);
            check(side->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            host.begin();host.commands->CopyResource(texture.Get(),other.Get());host.execute();
            require(probe.snapshot_status().foreign_submissions==0 && !probe.latest(),"without a target the submit hook records no foreign-queue or writer evidence");
        }
        require(probe.target(texture.Get()),"target resource is leased on its validated device");
        D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;desc.NumDescriptors=1;
        ComPtr<ID3D12DescriptorHeap> heap;check(host.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&heap)));
        const auto handle=heap->GetCPUDescriptorHandleForHeapStart();host.device->CreateRenderTargetView(texture.Get(),nullptr,handle);
        host.begin();require(!GpuClosedListProbe::read(host.commands.Get()),"actual Reset invalidates the previous Close receipt before reuse");
        host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);host.execute();
        const auto actual_close=GpuClosedListProbe::read(host.commands.Get());
        require(actual_close && actual_close->type==D3D12_COMMAND_LIST_TYPE_DIRECT && !GpuClosedListProbe::get().errors(),"native Close is observed once through early and late adoption");
        require(!probe.latest(),"RTV binding alone is not proof of a graphics write");
        ComPtr<ID3D12Fence> gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)));
        check(host.queue->Wait(gate.Get(),1));
        host.begin();auto transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
        const float rgba[]{.1f,.2f,.3f,1};host.commands->ClearRenderTargetView(handle,rgba,0,nullptr);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);check(host.commands->Close());
        ID3D12CommandList* lists[]{host.commands.Get()};host.queue->ExecuteCommandLists(1,lists);
        const bool incomplete=!probe.latest();
        check(gate->Signal(1));
        check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        require(incomplete,"queued native writer remains unavailable while its actual GPU fence is blocked");
        const auto completed=probe.latest();
        require(completed && completed->writes==1 && completed->resource==reinterpret_cast<uintptr_t>(texture.Get()) && completed->queue==reinterpret_cast<uintptr_t>(host.queue.Get()),"real clear writer is linked to its actual submitting queue and completed fence");
        require(completed->frequency>0 && completed->gpu_end>=completed->gpu_begin && completed->gpu_ms>=0 && completed->completion_upper_ms>=0,"resolved GPU timestamps are only exposed after fence completion");
        const auto state_coverage=probe.snapshot_status();
        require(state_coverage.barrier_calls>=2 && state_coverage.target_transitions>=2 && !state_coverage.untracked_transitions,
                "state diagnostics count actual target transitions independently of writer-batch transitions");
        require(completed->kinds[static_cast<size_t>(GpuCompletionProbe::Kind::clear)]==1 && completed->last_write_callsite && completed->execute_callsite && completed->last_write_thread==GetCurrentThreadId(),"completed evidence classifies the real writer and captures its recording/submission origins");
        UINT receipt_size{};check(host.commands->GetPrivateData(GpuCompletionProbe::RECORDING_KEY,&receipt_size,nullptr));
        std::vector<uint8_t> recording_receipt(receipt_size);check(host.commands->GetPrivateData(GpuCompletionProbe::RECORDING_KEY,&receipt_size,recording_receipt.data()));
        require(receipt_size>0,"actual Reset stamps the command object, without retaining it");
        check(host.commands->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,0,nullptr));
        host.queue->ExecuteCommandLists(1,lists);check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        require(probe.latest()->serial==completed->serial,"missing object-owned receipt cannot reuse an address-keyed closed recording");
        ComPtr<ID3D12CommandAllocator> receipt_allocator;ComPtr<ID3D12GraphicsCommandList> receipt_other;
        check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&receipt_allocator)));
        check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,receipt_allocator.Get(),nullptr,IID_PPV_ARGS(&receipt_other)));
        check(receipt_other->Close());check(receipt_allocator->Reset());check(receipt_other->Reset(receipt_allocator.Get(),nullptr));
        transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);receipt_other->ResourceBarrier(1,&transition);
        receipt_other->ClearRenderTargetView(handle,rgba,0,nullptr);std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);receipt_other->ResourceBarrier(1,&transition);check(receipt_other->Close());
        check(receipt_other->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,receipt_size,recording_receipt.data()));
        ID3D12CommandList* spoofed[]{receipt_other.Get()};host.queue->ExecuteCommandLists(1,spoofed);check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        require(probe.latest()->serial==completed->serial,"copying another object's recording receipt cannot forge a native generation");
        host.begin();host.commands->OMSetRenderTargets(1,&handle,TRUE,nullptr);host.commands->OMSetRenderTargets(0,nullptr,FALSE,nullptr);host.execute();
        require(probe.latest()->serial==completed->serial,"reset and binding changes cannot reuse a stale write generation");
        host.device->CreateRenderTargetView(other.Get(),nullptr,handle);
        host.begin();transition=barrier(other.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
        host.commands->ClearRenderTargetView(handle,rgba,0,nullptr);std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);host.execute();
        require(probe.latest()->serial==completed->serial,"descriptor reuse cannot attribute another resource's write to the owned target");
        host.begin();host.commands->CopyResource(texture.Get(),other.Get());host.execute();
        const auto copied=probe.latest();
        require(copied && copied->serial>completed->serial && copied->writes==1,"native resource-copy destination is linked to its completed queue batch");
        require(copied->kinds[static_cast<size_t>(GpuCompletionProbe::Kind::copy)]==1 && copied->kinds[static_cast<size_t>(GpuCompletionProbe::Kind::clear)]==0,"writer kinds reset with each native command-list generation");
        D3D12_TEXTURE_COPY_LOCATION destination{},source{};destination.pResource=texture.Get();source.pResource=other.Get();
        destination.Type=source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        host.begin();host.commands->CopyTextureRegion(&destination,0,0,0,&source,nullptr);host.execute();
        const auto copied_texture=probe.latest();
        require(copied_texture && copied_texture->serial>copied->serial && copied_texture->writes==1,"native texture-region destination is tracked independently of bound RTVs");
        D3D12_COMMAND_QUEUE_DESC queue_desc{};queue_desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> different;check(host.device->CreateCommandQueue(&queue_desc,IID_PPV_ARGS(&different)));
        host.begin();host.commands->CopyResource(texture.Get(),other.Get());check(host.commands->Close());different->ExecuteCommandLists(1,lists);
        check(different->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        require(probe.latest()->serial==copied_texture->serial,"work submitted on a different direct queue cannot inherit validated-queue completion evidence");
        require(probe.snapshot_status().foreign_submissions==1,"state diagnostics identify foreign-queue invalidation");
        host.begin();host.commands->CopyResource(texture.Get(),other.Get());host.execute();
        const auto after_foreign=probe.latest();
        require(after_foreign && (after_foreign->barrier_hazards&GpuStateEvidence::foreign_target_writer),"a later primary-queue write cannot erase an observed foreign target writer");
        std::vector<GpuTextureReadback::Pixels> refused_pixels;GpuCompletionProbe::Readback refused_receipt;
        const std::array<GpuTextureReadback::Source,1> refused_sources{{{texture.Get(),D3D12_RESOURCE_STATE_COMMON}}};
        require(probe.readback(after_foreign->serial,refused_sources,refused_pixels,refused_receipt)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && refused_pixels.empty(),"foreign target writer refuses a later apparently complete primary readback");
        probe.clear_target();require(probe.target(texture.Get()),"fixture starts a new observation epoch after proving sticky foreign-writer refusal");
        {
            D3D12_COMMAND_QUEUE_DESC copy_desc{};copy_desc.Type=D3D12_COMMAND_LIST_TYPE_COPY;
            ComPtr<ID3D12CommandQueue> copy_queue;ComPtr<ID3D12CommandAllocator> copy_allocator;ComPtr<ID3D12GraphicsCommandList> copy_commands;
            check(host.device->CreateCommandQueue(&copy_desc,IID_PPV_ARGS(&copy_queue)));
            check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&copy_allocator)));
            check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&copy_commands)));
            check(copy_commands->Close());check(copy_commands->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,0,nullptr));
            check(copy_commands->SetPrivateData(GpuClosedListProbe::KEY,0,nullptr)); // No closure proof: synchronous recovery must refuse.
            ID3D12CommandList* unknown_lists[]{copy_commands.Get()};copy_queue->ExecuteCommandLists(1,unknown_lists);
            check(copy_queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            const auto foreign=probe.snapshot_status().foreign_list;
            require(foreign.ordinal && foreign.reason && foreign.type==D3D12_COMMAND_LIST_TYPE_COPY &&
                foreign.queue==reinterpret_cast<uintptr_t>(copy_queue.Get()) && foreign.commands==reinterpret_cast<uintptr_t>(copy_commands.Get()) &&
                foreign.graphics && foreign.identity && foreign.reset && foreign.close && foreign.callsite && foreign.thread,
                "actual unknown foreign copy list retains its type, object/interface, implementation and missing-lifecycle diagnostics");
        }
        host.begin();host.commands->CopyResource(texture.Get(),other.Get());host.execute();const auto after_unknown_foreign=probe.latest();
        require(after_unknown_foreign && (after_unknown_foreign->barrier_hazards&GpuStateEvidence::unknown_foreign_list),"unknown foreign list remains unsafe after a later primary target write");
        require(probe.readback(after_unknown_foreign->serial,refused_sources,refused_pixels,refused_receipt)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"unknown foreign list cannot inherit primary completion evidence");
        probe.clear_target();require(probe.target(texture.Get()),"new fixture epoch after unknown foreign refusal");
        {
            D3D12_COMMAND_QUEUE_DESC copy_desc{};copy_desc.Type=D3D12_COMMAND_LIST_TYPE_COPY;
            ComPtr<ID3D12CommandQueue> copy_queue;ComPtr<ID3D12CommandAllocator> copy_allocator;ComPtr<ID3D12GraphicsCommandList> copy_commands;
            check(host.device->CreateCommandQueue(&copy_desc,IID_PPV_ARGS(&copy_queue)));check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&copy_allocator)));
            check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&copy_commands)));
            check(copy_commands->Close());check(copy_commands->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,0,nullptr));
            // This fixture target predates allocation observation, so it cannot
            // qualify for private-target recovery. Missing proof stays refused.
            ID3D12CommandList* unknown_lists[]{copy_commands.Get()};copy_queue->ExecuteCommandLists(1,unknown_lists);
            require(probe.snapshot_status().foreign_fence_failures==1 && !probe.snapshot_status().foreign_fenced_lists,"no private allocation proof cannot enable foreign completion recovery");
            probe.clear_target();require(probe.target(texture.Get()),"new fixture epoch after missing-private-proof refusal");
        }
        {
            ComPtr<ID3D12CommandAllocator> untracked_allocator;ComPtr<ID3D12GraphicsCommandList> untracked_commands;
            check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&untracked_allocator)));
            check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,untracked_allocator.Get(),nullptr,IID_PPV_ARGS(&untracked_commands)));
            check(untracked_commands->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,0,nullptr));
            untracked_commands->CopyResource(texture.Get(),other.Get());check(untracked_commands->Close());
            ID3D12CommandList* untracked_lists[]{untracked_commands.Get()};host.queue->ExecuteCommandLists(1,untracked_lists);
            check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            std::cout<<"untracked_target_writes="<<probe.snapshot_status().untracked_target_writes<<std::endl;
            require(probe.snapshot_status().untracked_target_writes>=1,"actual explicit target copy without a valid Reset receipt retains a missing-generation writer failure");
            host.begin();host.commands->CopyResource(texture.Get(),other.Get());host.execute();const auto after_untracked=probe.latest();
            require(after_untracked && (after_untracked->barrier_hazards&GpuStateEvidence::untracked_target_writer),"later primary write cannot erase an untracked explicit target writer");
            require(probe.readback(after_untracked->serial,refused_sources,refused_pixels,refused_receipt)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),"untracked explicit writer prevents successful readback");
            probe.clear_target();require(probe.target(texture.Get()),"fixture starts a new epoch after sticky untracked-writer proof");
        }
        // Exercise an actual raster draw through an RTV that exists only by copy.
        ComPtr<ID3DBlob> root_blob,error,vs,ps;ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12PipelineState> pipeline;
        D3D12_ROOT_SIGNATURE_DESC root_desc{};root_desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        check(D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&root_blob,&error));
        check(host.device->CreateRootSignature(0,root_blob->GetBufferPointer(),root_blob->GetBufferSize(),IID_PPV_ARGS(&root)));
        constexpr char shader[]=R"(float4 vs(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);} float4 ps():SV_Target {return float4(.4,.3,.2,1);})";
        check(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vs,&error));
        check(D3DCompile(shader,sizeof(shader)-1,nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&ps,&error));
        D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};state.pRootSignature=root.Get();
        state.VS={vs->GetBufferPointer(),vs->GetBufferSize()};state.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        state.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;state.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;state.RasterizerState.DepthClipEnable=TRUE;
        state.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
        state.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        state.SampleMask=UINT_MAX;state.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        state.NumRenderTargets=1;state.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;state.SampleDesc.Count=1;
        check(host.device->CreateGraphicsPipelineState(&state,IID_PPV_ARGS(&pipeline)));
        host.device->CreateRenderTargetView(texture.Get(),nullptr,handle);
        ComPtr<ID3D12DescriptorHeap> alias;check(host.device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&alias)));
        const auto alias_handle=alias->GetCPUDescriptorHandleForHeapStart();host.device->CopyDescriptorsSimple(1,alias_handle,handle,D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        writer_origin=GpuCompletionProbe::Origin{17,0,0};
        writer_origin->source_world=writer_origin->render_world={1,0,0,0,0,1,0,0,0,0,-1,0,10,20,30,1};
        const auto draw_generation=[&](bool transitions=true) {
        host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);if(transitions) host.commands->ResourceBarrier(1,&transition);
        host.commands->SetPipelineState(pipeline.Get());host.commands->SetGraphicsRootSignature(root.Get());host.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VIEWPORT viewport{0,0,8,4,0,1};const D3D12_RECT rectangle{0,0,8,4};
        host.commands->RSSetViewports(1,&viewport);host.commands->RSSetScissorRects(1,&rectangle);host.commands->OMSetRenderTargets(1,&alias_handle,TRUE,nullptr);
        host.commands->DrawInstanced(3,1,0,0);
        D3D12_HEAP_PROPERTIES upload{};upload.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC index_desc{};index_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;index_desc.Width=6;
        index_desc.Height=index_desc.DepthOrArraySize=index_desc.MipLevels=1;index_desc.SampleDesc.Count=1;index_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> index_buffer;check(host.device->CreateCommittedResource(&upload,D3D12_HEAP_FLAG_NONE,&index_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&index_buffer)));
        void* mapped{};D3D12_RANGE no_read{};check(index_buffer->Map(0,&no_read,&mapped));const uint16_t indices[]{0,1,2};std::memcpy(mapped,indices,sizeof(indices));index_buffer->Unmap(0,nullptr);
        const D3D12_INDEX_BUFFER_VIEW index_view{index_buffer->GetGPUVirtualAddress(),6,DXGI_FORMAT_R16_UINT};
        host.commands->IASetIndexBuffer(&index_view);host.commands->DrawIndexedInstanced(3,1,0,0,0);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);if(transitions) host.commands->ResourceBarrier(1,&transition);host.execute();
        };
        draw_generation();
        const auto raster=probe.latest();
        require(raster && raster->serial>copied_texture->serial && raster->writes==2 && raster->kinds[static_cast<size_t>(GpuCompletionProbe::Kind::draw)]==1 && raster->kinds[static_cast<size_t>(GpuCompletionProbe::Kind::indexed)]==1,"actual completed direct and indexed raster draws through copied RTV are attributed to their target");
        require(raster->origin && raster->origin->render_id==17 && raster->origin->token==0,"unique camera source survives real recording, submission and GPU fence completion, including token zero");
        {
            std::vector<GpuTextureReadback::Pixels> pixels;GpuCompletionProbe::Readback receipt;
            const std::array<GpuTextureReadback::Source,1> sources{{{texture.Get(),D3D12_RESOURCE_STATE_COMMON}}};
            const std::array<GpuTextureReadback::Source,1> invalid_state{{{texture.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE}}};
            require(probe.readback(raster->serial,invalid_state,pixels,receipt)==E_INVALIDARG && pixels.empty() &&
                    !receipt.fence_completed && !probe.readback_active(),"failed copy preparation clears the guard and exposes no successful receipt");
            const auto device_queue_receipt=*GpuQueueProbe::read_device(host.device.Get());auto changed_methods=device_queue_receipt;
            changed_methods.execute_methods[0]^=4;check(host.device->SetPrivateData(GpuQueueProbe::DEVICE_KEY,sizeof(changed_methods),&changed_methods));
            require(probe.readback(raster->serial,sources,pixels,receipt)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && pixels.empty(),"a different observed queue execution implementation refuses even an otherwise completed writer");
            check(host.device->SetPrivateData(GpuQueueProbe::DEVICE_KEY,sizeof(device_queue_receipt),&device_queue_receipt));
            const auto primary_queue_receipt=*GpuQueueProbe::read(host.queue.Get());check(host.queue->SetPrivateData(GpuQueueProbe::QUEUE_KEY,0,nullptr));
            require(probe.readback(raster->serial,sources,pixels,receipt)==HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) && pixels.empty(),"missing actual primary queue creation cannot be inferred from its completed fence");
            check(host.queue->SetPrivateData(GpuQueueProbe::QUEUE_KEY,sizeof(primary_queue_receipt),&primary_queue_receipt));
            require(probe.readback(raster->serial,sources,pixels,receipt)==S_OK && pixels.size()==1,
                    "real fenced readback can recover at the same boundary after preparation failure and re-enter its own command hooks without deadlocking");
            require(receipt.fence_completed && receipt.writer_serial==raster->serial && receipt.writer_submission==raster->submission_sequence &&
                    receipt.copy_submission==receipt.writer_submission+1 && receipt.executions==1 && receipt.commands &&
                    receipt.queue==raster->queue && receipt.resource==raster->resource,"receipt binds the actual copy and its fence to the immediately preceding observed submission");
            for(size_t i=0;i<pixels[0].rgb.size();i+=3) require(pixels[0].rgb[i]==102 && std::abs(int(pixels[0].rgb[i+1])-77)<=1 && pixels[0].rgb[i+2]==51,"copied bytes contain the actual raster output");
            require(probe.readback(raster->serial,sources,pixels,receipt)==HRESULT_FROM_WIN32(ERROR_RETRY) && pixels.empty() && !receipt.fence_completed,
                    "even a previous diagnostic copy makes the old native boundary stale");
            draw_generation();const auto before_empty=probe.latest();
            host.begin();host.execute();
            require(probe.readback(before_empty->serial,sources,pixels,receipt)==HRESULT_FROM_WIN32(ERROR_RETRY),
                    "an unattributed subsequent submission cannot silently inherit an old writer boundary");
            draw_generation();const auto before_other=probe.latest();
            host.begin();check(host.commands->Close());different->ExecuteCommandLists(1,lists);
            check(different->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            require(probe.readback(before_other->serial,sources,pixels,receipt)==HRESULT_FROM_WIN32(ERROR_RETRY),
                    "a different hooked queue invalidates old boundary evidence even without an attributed write");
            draw_generation();const auto before_pending=probe.latest();
            ComPtr<ID3D12Fence> blocked;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocked)));
            check(host.queue->Wait(blocked.Get(),1));
            host.begin();host.commands->CopyResource(texture.Get(),other.Get());check(host.commands->Close());host.queue->ExecuteCommandLists(1,lists);
            const auto pending=probe.readback(before_pending->serial,sources,pixels,receipt);
            check(blocked->Signal(1));check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            require(pending==HRESULT_FROM_WIN32(ERROR_RETRY) && pixels.empty() && !receipt.fence_completed,
                    "a newer queued but incomplete target writer prevents stale readback attribution");

            // Block the GPU copy, then attempt a real second-queue submission.
            // The observer's active flag and event coordinate entry; the bounded
            // wait checks exclusion while the copy's fence is still unsignalled.
            draw_generation();const auto concurrent=probe.latest();
            ComPtr<ID3D12Fence> copy_gate;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copy_gate)));
            check(host.queue->Wait(copy_gate.Get(),1));
            host.begin();check(host.commands->Close());
            auto capture=std::async(std::launch::async,[&]{return probe.readback(concurrent->serial,sources,pixels,receipt);});
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
            while(!probe.readback_active() && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
            const bool active=probe.readback_active();
            std::promise<void> entering;auto entered=entering.get_future();
            auto competing=std::async(std::launch::async,[&]{entering.set_value();different->ExecuteCommandLists(1,lists);});
            entered.wait();const bool excluded=competing.wait_for(std::chrono::milliseconds(50))==std::future_status::timeout;
            check(copy_gate->Signal(1));const auto captured=capture.get();competing.get();
            check(different->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            require(active && excluded && captured==S_OK && receipt.fence_completed && !probe.readback_active(),
                    "competing queue submission waits until the guarded GPU copy and fence finish");
            require(probe.readback(concurrent->serial,sources,pixels,receipt)==HRESULT_FROM_WIN32(ERROR_RETRY),"competing submission invalidates the old boundary after the guard exits");
            const std::array<GpuTextureReadback::Source,1> wrong{{{other.Get(),D3D12_RESOURCE_STATE_COMMON}}};
            require(probe.readback(concurrent->serial,wrong,pixels,receipt)==E_INVALIDARG && pixels.empty() && !receipt.fence_completed,
                    "a different resource cannot acquire the owned target's receipt");
        }
        require(probe.request_snapshot(21),"arm an owned-target snapshot for the next actual writer batch");
        host.begin();host.execute();require(!probe.take_snapshot(21),"an empty native generation cannot produce a writer snapshot");
        draw_generation();auto snapshot=probe.take_snapshot(21);
        require(snapshot && snapshot->result==S_OK && snapshot->pixels.size()==1 && snapshot->writer.origin &&
                snapshot->writer.origin->render_id==17 && snapshot->boundary.fence_completed &&
                snapshot->boundary.writer_submission==snapshot->writer.submission_sequence &&
                snapshot->boundary.copy_submission==snapshot->writer.submission_sequence+1,
                "snapshot is copied immediately after the native writer and carries its completed timestamp/source evidence");
        for(size_t i=0;i<snapshot->pixels[0].rgb.size();i+=3) require(snapshot->pixels[0].rgb[i]==102,"snapshot bytes retain the real raster output");
        host.begin();host.commands->CopyResource(texture.Get(),other.Get());host.execute();
        require(snapshot->pixels[0].rgb[0]==102,"later native writes cannot change an already fenced CPU snapshot");
        require(!probe.take_snapshot(21),"a completed snapshot can only be consumed once");
        require(probe.request_snapshot(22),"arm a pending snapshot before invalidation");probe.clear_target();
        require(!probe.take_snapshot(22) && !probe.request_snapshot(22),"target invalidation cancels pending snapshot ownership");
        require(probe.target(texture.Get()),"restore the fixture target after invalidation");
        require(probe.request_snapshot(23),"arm a snapshot that needs a verified final transition state");
        host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
        transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition.Flags=D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;host.commands->ResourceBarrier(1,&transition);
        transition.Flags=D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;host.commands->ResourceBarrier(1,&transition);host.execute();
        require(!probe.take_snapshot(23),"split transitions deliberately refuse state inference for a snapshot");
        host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);host.execute();
        draw_generation();auto recovered=probe.take_snapshot(23);
        require(recovered && recovered->result==S_OK && recovered->boundary.source_state==D3D12_RESOURCE_STATE_COMMON,
                "a subsequent full transition restores the known copy state and services the pending snapshot");
        ComPtr<ID3D12CommandAllocator> empty_allocator;ComPtr<ID3D12GraphicsCommandList> empty_list;
        check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&empty_allocator)));
        check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,empty_allocator.Get(),nullptr,IID_PPV_ARGS(&empty_list)));check(empty_list->Close());
        check(empty_list->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,0,nullptr)); // Deliberately unknown despite the new creation observer.
        require(probe.request_snapshot(24),"arm a snapshot before a multi-list native batch");
        host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);check(host.commands->Close());
        ID3D12CommandList* overlapping[]{host.commands.Get(),empty_list.Get()};host.queue->ExecuteCommandLists(2,overlapping);
        check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        require(!probe.take_snapshot(24) && probe.snapshot_status().state==UINT32_MAX,
                "an unknown list in a multi-list submission cannot establish the target's ending state");
        draw_generation();const auto single_list=probe.take_snapshot(24);
        require(single_list && single_list->result==S_OK,"a later single-list full transition recovers the pending snapshot");
        ComPtr<ID3D12GraphicsCommandList> initial_recording;
        check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,empty_allocator.Get(),nullptr,IID_PPV_ARGS(&initial_recording)));
        check(initial_recording->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,0,nullptr)); // Exercise missing provenance, rather than assuming Create is unobserved.
        transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);initial_recording->ResourceBarrier(1,&transition);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);initial_recording->ResourceBarrier(1,&transition);check(initial_recording->Close());
        const auto untracked=probe.snapshot_status();
        require(untracked.untracked_transitions>=2,"diagnostics distinguish transitions recorded before an observed Reset from missing hook traffic");
        ID3D12CommandList* initial_lists[]{initial_recording.Get()};host.queue->ExecuteCommandLists(1,initial_lists);
        check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        check(empty_allocator->Reset());check(empty_list->Reset(empty_allocator.Get(),nullptr));check(empty_list->Close());
        require(probe.request_snapshot(25),"arm a snapshot for one target list batched with a known empty list");
        host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);check(host.commands->Close());
        host.queue->ExecuteCommandLists(2,overlapping);check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        const auto batched=probe.take_snapshot(25);
        require(batched && batched->result==S_OK && batched->writer.lists==2 && batched->writer.target_lists==1 && batched->writer.all_lists_tracked,
                "known unrelated lists do not make one observed target list's ending state ambiguous");
        check(empty_allocator->Reset());check(empty_list->Reset(empty_allocator.Get(),nullptr));
        transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);empty_list->ResourceBarrier(1,&transition);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);empty_list->ResourceBarrier(1,&transition);check(empty_list->Close());
        require(probe.request_snapshot(28),"arm a snapshot before two known lists touch the target");
        host.queue->ExecuteCommandLists(2,overlapping);check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        require(!probe.take_snapshot(28),"multiple target-touching lists still refuse until transition-chain proof is implemented");
        // Put the resource in RT before observing it, as with a pooled native output.
        host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);host.execute();
        probe.clear_target();require(probe.target(texture.Get()) && probe.request_snapshot(26),"observe a pooled target after its initial transition");
        draw_generation(false);const auto pooled=probe.take_snapshot(26);
        require(pooled && pooled->result==S_OK && pooled->boundary.source_state==D3D12_RESOURCE_STATE_RENDER_TARGET && pooled->boundary.raster_state &&
                !pooled->boundary.common_decay && probe.snapshot_status().target_transitions==0,
                "actual RTV raster access establishes state without inventing a missed creation transition");
        host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&transition);host.execute();
        auto simultaneous_desc=texture->GetDesc();simultaneous_desc.Flags|=D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_PROPERTIES simultaneous_heap{};simultaneous_heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> simultaneous;
        check(host.device->CreateCommittedResource(&simultaneous_heap,D3D12_HEAP_FLAG_NONE,&simultaneous_desc,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&simultaneous)));
        host.device->CreateRenderTargetView(simultaneous.Get(),nullptr,alias_handle);
        require(probe.target(simultaneous.Get()) && probe.request_snapshot(27),"observe an actual simultaneous-access texture");
        draw_generation(false);const auto decayed=probe.take_snapshot(27);
        require(decayed && decayed->result==S_OK && decayed->boundary.source_state==D3D12_RESOURCE_STATE_COMMON && decayed->boundary.raster_state &&
                decayed->boundary.common_decay && decayed->pixels[0].rgb[0]==102,
                "actual simultaneous-access raster output decays to COMMON before the adjacent copy");
        host.device->CreateRenderTargetView(texture.Get(),nullptr,alias_handle);require(probe.target(texture.Get()),"restore ordinary fixture target after decay control");
        require(probe.request_snapshot(28),"arm an alias-guarded writer snapshot");
        host.begin();D3D12_RESOURCE_BARRIER global_alias{};global_alias.Type=D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        host.commands->ResourceBarrier(1,&global_alias);
        transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
        std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);host.execute();
        require(!probe.take_snapshot(28) && probe.snapshot_status().state==UINT32_MAX &&
            (probe.snapshot_status().barrier_hazards&GpuStateEvidence::aliasing) && probe.snapshot_status().aliasing_barriers,
            "real alias barrier taints the writer even after a full transition and actual clear");
        const auto alias_evidence=probe.snapshot_status();
        std::cout<<"alias_hook_observations="<<alias_evidence.aliasing_barriers<<" both_null="<<alias_evidence.alias_both_null<<" sampled_ordinal="<<alias_evidence.alias_sample.ordinal<<'\n';
        require(alias_evidence.alias_both_null>0 && alias_evidence.alias_both_null==alias_evidence.aliasing_barriers && !alias_evidence.alias_target_named && !alias_evidence.alias_other &&
            alias_evidence.alias_sample.ordinal==1 && !alias_evidence.alias_sample.before && !alias_evidence.alias_sample.after &&
            alias_evidence.alias_sample.callsite && !alias_evidence.allocation,"alias diagnostic sampling preserves wildcard shape without inventing pre-observation target placement");
        draw_generation();const auto alias_recovered=probe.take_snapshot(28);
        require(alias_recovered && alias_recovered->result==S_OK && !alias_recovered->boundary.barrier_hazards &&
            alias_recovered->boundary.barrier_capabilities_known &&
            (!alias_recovered->boundary.enhanced_supported || alias_recovered->boundary.enhanced_hook_installed),
            "a fresh recording can establish its own state and exports actual enhanced-observer capability evidence");
        if(probe.barrier_capabilities().enhanced_supported) {
            ComPtr<ID3D12GraphicsCommandList7> extended;check(host.commands.As(&extended));
            require(probe.request_snapshot(29),"arm an enhanced-barrier-guarded snapshot");
            host.begin();transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
            host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
            std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);
            D3D12_GLOBAL_BARRIER global{};global.SyncBefore=global.SyncAfter=D3D12_BARRIER_SYNC_ALL;
            global.AccessBefore=global.AccessAfter=D3D12_BARRIER_ACCESS_COMMON;
            D3D12_BARRIER_GROUP group{};group.Type=D3D12_BARRIER_TYPE_GLOBAL;group.NumBarriers=1;group.pGlobalBarriers=&global;
            extended->Barrier(1,&group);host.execute();
            require(!probe.take_snapshot(29) && (probe.snapshot_status().barrier_hazards&GpuStateEvidence::enhanced) && probe.snapshot_status().enhanced_calls,
                "actual enhanced API call is observed and prevents copying through unmodeled barrier semantics");
            draw_generation();const auto enhanced_recovered=probe.take_snapshot(29);
            require(enhanced_recovered && enhanced_recovered->result==S_OK,"a subsequent fresh legacy recording can service the guarded request");
            std::cout<<"enhanced_barrier_runtime_case_exercised=true\n";
        } else std::cout<<"enhanced_barrier_runtime_case_exercised=false (device reports unsupported)\n";
        // Real base-API interception: even harmless/unrelated examples cannot
        // prove target exclusion without descriptor/signature/heap ownership.
        ComPtr<ID3DBlob> cs;constexpr char compute_shader[]="[numthreads(1,1,1)] void cs() {}";
        check(D3DCompile(compute_shader,sizeof(compute_shader)-1,nullptr,nullptr,nullptr,"cs","cs_5_0",0,0,&cs,&error));
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{};compute_desc.pRootSignature=root.Get();compute_desc.CS={cs->GetBufferPointer(),cs->GetBufferSize()};
        ComPtr<ID3D12PipelineState> compute;check(host.device->CreateComputePipelineState(&compute_desc,IID_PPV_ARGS(&compute)));
        D3D12_HEAP_PROPERTIES upload_heap{};upload_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer_desc{};buffer_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer_desc.Width=256;
        buffer_desc.Height=buffer_desc.DepthOrArraySize=buffer_desc.MipLevels=1;buffer_desc.SampleDesc.Count=1;buffer_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> arguments;check(host.device->CreateCommittedResource(&upload_heap,D3D12_HEAP_FLAG_NONE,&buffer_desc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&arguments)));
        void* mapped_arguments{};check(arguments->Map(0,nullptr,&mapped_arguments));const D3D12_DISPATCH_ARGUMENTS dispatch_arguments{1,1,1};
        std::memcpy(mapped_arguments,&dispatch_arguments,sizeof(dispatch_arguments));arguments->Unmap(0,nullptr);
        D3D12_INDIRECT_ARGUMENT_DESC argument_desc{};argument_desc.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC signature_desc{};signature_desc.ByteStride=sizeof(dispatch_arguments);signature_desc.NumArgumentDescs=1;signature_desc.pArgumentDescs=&argument_desc;
        ComPtr<ID3D12CommandSignature> signature;check(host.device->CreateCommandSignature(&signature_desc,nullptr,IID_PPV_ARGS(&signature)));
        ComPtr<ID3D12CommandAllocator> bundle_allocator;ComPtr<ID3D12GraphicsCommandList> bundle;
        check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE,IID_PPV_ARGS(&bundle_allocator)));
        check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_BUNDLE,bundle_allocator.Get(),nullptr,IID_PPV_ARGS(&bundle)));check(bundle->Close());
        D3D12_HEAP_PROPERTIES default_heap{};default_heap.Type=D3D12_HEAP_TYPE_DEFAULT;buffer_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> uav_buffer;check(host.device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&buffer_desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&uav_buffer)));
        D3D12_DESCRIPTOR_HEAP_DESC uav_heap_desc{};uav_heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;uav_heap_desc.NumDescriptors=2;
        ComPtr<ID3D12DescriptorHeap> uav_cpu,uav_gpu;check(host.device->CreateDescriptorHeap(&uav_heap_desc,IID_PPV_ARGS(&uav_cpu)));
        uav_heap_desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;check(host.device->CreateDescriptorHeap(&uav_heap_desc,IID_PPV_ARGS(&uav_gpu)));
        const auto descriptor_step=host.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for(UINT i=0;i<2;++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=i?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R32_UINT;
            view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;view.Buffer.NumElements=64;
            auto cpu=uav_cpu->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=i*descriptor_step;
            auto gpu_cpu=uav_gpu->GetCPUDescriptorHandleForHeapStart();gpu_cpu.ptr+=i*descriptor_step;
            host.device->CreateUnorderedAccessView(uav_buffer.Get(),nullptr,&view,cpu);
            host.device->CreateUnorderedAccessView(uav_buffer.Get(),nullptr,&view,gpu_cpu);
        }
        for(size_t kind=0;kind<5;++kind) {
            const auto before=probe.snapshot_status().unsupported_writer_calls;
            const auto tag=uint64_t(40+kind);require(probe.request_snapshot(tag),"arm base-writer guard control");
            host.begin();
            if(kind<=1) {
                host.commands->SetPipelineState(compute.Get());host.commands->SetComputeRootSignature(root.Get());
                if(kind==0) host.commands->Dispatch(1,1,1);
                else host.commands->ExecuteIndirect(signature.Get(),1,arguments.Get(),0,nullptr,0);
            } else if(kind==2) host.commands->ExecuteBundle(bundle.Get());
            else {
                ID3D12DescriptorHeap* heaps[]{uav_gpu.Get()};host.commands->SetDescriptorHeaps(1,heaps);
                const auto index=UINT(kind-3);auto gpu=uav_gpu->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=index*descriptor_step;
                auto cpu=uav_cpu->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=index*descriptor_step;
                if(kind==3) {const UINT values[]{1,2,3,4};host.commands->ClearUnorderedAccessViewUint(gpu,cpu,uav_buffer.Get(),values,0,nullptr);}
                else {const FLOAT values[]{1,2,3,4};host.commands->ClearUnorderedAccessViewFloat(gpu,cpu,uav_buffer.Get(),values,0,nullptr);}
            }
            transition=barrier(texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&transition);
            host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
            std::swap(transition.Transition.StateBefore,transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&transition);host.execute();
            const auto status=probe.snapshot_status();
            std::cout<<"base_writer_case="<<kind<<" counts_before=";
            for(auto n:before) std::cout<<n<<',';
            std::cout<<" counts_after=";for(auto n:status.unsupported_writer_calls) std::cout<<n<<',';
            std::cout<<" last_kind="<<status.unsupported_writer_kind<<'\n';
            std::cout<<"nested_writer_counts=";for(auto n:status.nested_writer_calls) std::cout<<n<<',';std::cout<<'\n';
            require(!probe.take_snapshot(tag) && status.state==UINT32_MAX && (status.barrier_hazards&GpuStateEvidence::unsupported_writer),
                "an actual base writer prevents snapshot even after a target clear/full transition");
            for(size_t i=0;i<5;++i) require(status.unsupported_writer_calls[i]==before[i]+(i==kind),"each actual writer API is intercepted/classified exactly once");
            require(status.unsupported_writer_callsite && status.unsupported_writer_kind==(1u<<kind) && status.unsupported_writer_thread==GetCurrentThreadId(),"writer guard retains recording origin");
            draw_generation();const auto recovered=probe.take_snapshot(tag);
            require(recovered && recovered->result==S_OK && recovered->boundary.base_writer_hook_mask==31,"fresh known raster recording recovers and exports complete base-hook receipt");
        }
        std::cout<<"base_writer_runtime_cases_exercised=5\n";
        D3D12_COMMAND_QUEUE_DESC old_copy_desc{};old_copy_desc.Type=D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> old_copy_queue;ComPtr<ID3D12CommandAllocator> old_copy_allocator;ComPtr<ID3D12GraphicsCommandList> old_copy_commands;
        check(host.device->CreateCommandQueue(&old_copy_desc,IID_PPV_ARGS(&old_copy_queue)));
        check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&old_copy_allocator)));
        check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,old_copy_allocator.Get(),nullptr,IID_PPV_ARGS(&old_copy_commands)));check(old_copy_commands->Close());
        auto private_texture=host.color(.2f,.3f,.4f);
        require(probe.target(private_texture.Get()),"observe a target created after actual allocation hooks");
        require(probe.snapshot_status().allocation && probe.snapshot_status().allocation->access().private_render_target(),"private target proof uses actual heap properties");
        {
            D3D12_COMMAND_QUEUE_DESC copy_desc{};copy_desc.Type=D3D12_COMMAND_LIST_TYPE_COPY;
            ComPtr<ID3D12CommandQueue> copy_queue;ComPtr<ID3D12CommandAllocator> copy_allocator;ComPtr<ID3D12GraphicsCommandList> copy_commands;
            check(host.device->CreateCommandQueue(&copy_desc,IID_PPV_ARGS(&copy_queue)));check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY,IID_PPV_ARGS(&copy_allocator)));
            check(host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_COPY,copy_allocator.Get(),nullptr,IID_PPV_ARGS(&copy_commands)));
            check(copy_commands->Close());check(copy_commands->SetPrivateData(GpuCompletionProbe::RECORDING_KEY,0,nullptr));
            ID3D12CommandList* unknown_lists[]{copy_commands.Get()};copy_queue->ExecuteCommandLists(1,unknown_lists);
            const auto completion=probe.snapshot_status();require(completion.foreign_fenced_lists==1 && completion.foreign_fenced_submissions==1 && !completion.foreign_fence_failures && completion.foreign_fence_value==1 && completion.foreign_completed_value>=1,
                "actual unknown COPY submission completes its real queue fence before primary work can proceed");
            host.device->CreateRenderTargetView(private_texture.Get(),nullptr,alias_handle);host.begin();
            auto ordered_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&ordered_transition);
            host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
            std::swap(ordered_transition.Transition.StateBefore,ordered_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&ordered_transition);host.execute();
            const auto ordered_writer=probe.latest();std::vector<GpuTextureReadback::Pixels> ordered_pixels;GpuCompletionProbe::Readback ordered_boundary;
            const std::array<GpuTextureReadback::Source,1> ordered_sources{{{private_texture.Get(),D3D12_RESOURCE_STATE_COMMON}}};
            require(ordered_writer && !ordered_writer->barrier_hazards && probe.readback(ordered_writer->serial,ordered_sources,ordered_pixels,ordered_boundary)==S_OK && ordered_pixels.size()==1 && ordered_boundary.fence_completed,
                "actual completed foreign queue precedes a fresh primary RTV writer and guarded pixel copy without relaxing writer coverage");
            ComPtr<ID3D12Fence> blocker;check(host.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&blocker)));check(copy_queue->Wait(blocker.Get(),1));
            copy_queue->ExecuteCommandLists(1,unknown_lists);
            require(probe.snapshot_status().foreign_fence_failures==1 && probe.snapshot_status().foreign_fenced_lists==1,"blocked foreign queue times out without fabricating completion or losing its pending fence/event leases");
            check(blocker->Signal(1));check(copy_queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            probe.clear_target();require(probe.target(private_texture.Get()),"new fixture epoch after deliberate timeout refusal");
        }
        {
            ComPtr<ID3D12CommandAllocator> first_allocator;ComPtr<ID3D12GraphicsCommandList> first_commands;
            check(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&first_allocator)));
            auto* reset_address=(*reinterpret_cast<void***>(host.commands.Get()))[10];require(MH_DisableHook(reset_address)==MH_OK,"suppress fixture runtime Reset forwarding to exercise independent creation observation");
            const auto created=host.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,first_allocator.Get(),nullptr,IID_PPV_ARGS(&first_commands));
            require(MH_EnableHook(reset_address)==MH_OK,"restore native Reset observation immediately after fixture creation");check(created);
            host.device->CreateRenderTargetView(private_texture.Get(),nullptr,alias_handle);
            auto initial_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);first_commands->ResourceBarrier(1,&initial_transition);
            first_commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
            std::swap(initial_transition.Transition.StateBefore,initial_transition.Transition.StateAfter);first_commands->ResourceBarrier(1,&initial_transition);
            check(first_commands->Close());ID3D12CommandList* first_lists[]{first_commands.Get()};
            host.queue->ExecuteCommandLists(1,first_lists);check(host.queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
            const auto first_writer=probe.latest();std::cout<<"initial_creation_writer="<<bool(first_writer)<<" commands="<<(first_writer?first_writer->commands:0)<<" expected="<<reinterpret_cast<uintptr_t>(first_commands.Get())<<" writes="<<(first_writer?first_writer->writes:0)<<" hazards="<<(first_writer?first_writer->barrier_hazards:0)<<" birth="<<bool(GpuClosedListProbe::birth(first_commands.Get()))<<" observer_errors="<<GpuClosedListProbe::get().errors()<<'\n';require(first_writer && first_writer->commands==reinterpret_cast<uintptr_t>(first_commands.Get()) && first_writer->writes && !first_writer->barrier_hazards,
                "first native recording is attributed even when native creation never calls the public Reset hook");
        }
        ID3D12CommandList* old_copy_lists[]{old_copy_commands.Get()};old_copy_queue->ExecuteCommandLists(1,old_copy_lists);
        check(old_copy_queue->Signal(host.fence.Get(),++host.tick));wait(host.fence.Get(),host.tick);
        const auto old_copy_status=probe.snapshot_status();
        require(old_copy_status.foreign_unknown_lists==1 && old_copy_status.foreign_precreation_copies==1 && old_copy_status.foreign_list.precreation_copy && old_copy_status.foreign_list.closure &&
            old_copy_status.foreign_list.closure->closed_ns<old_copy_status.allocation->creation_begin_ns,"actual same-device COPY list closed before private target creation has an explicit exclusion receipt");
        host.device->CreateRenderTargetView(private_texture.Get(),nullptr,alias_handle);
        const auto private_generation=[&](bool indirect,ID3D12CommandSignature* override_signature=nullptr) {
            host.begin();host.commands->ResourceBarrier(1,&global_alias);
            host.commands->SetPipelineState(compute.Get());host.commands->SetComputeRootSignature(root.Get());
            if(indirect) host.commands->ExecuteIndirect(override_signature?override_signature:signature.Get(),1,arguments.Get(),0,nullptr,0);
            else host.commands->Dispatch(1,1,1);
            ID3D12DescriptorHeap* heaps[]{uav_gpu.Get()};host.commands->SetDescriptorHeaps(1,heaps);
            const UINT values[]{1,2,3,4};host.commands->ClearUnorderedAccessViewUint(uav_gpu->GetGPUDescriptorHandleForHeapStart(),uav_cpu->GetCPUDescriptorHandleForHeapStart(),uav_buffer.Get(),values,0,nullptr);
            D3D12_RESOURCE_BARRIER all_uavs{};all_uavs.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;host.commands->ResourceBarrier(1,&all_uavs);
            auto to_rt=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&to_rt);
            host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
            std::swap(to_rt.Transition.StateBefore,to_rt.Transition.StateAfter);host.commands->ResourceBarrier(1,&to_rt);host.execute();
        };
        require(probe.request_snapshot(50),"arm proven private access control");private_generation(false);
        const auto private_copy=probe.take_snapshot(50);
        require(private_copy && private_copy->result==S_OK && !private_copy->boundary.barrier_hazards && private_copy->boundary.allocation &&
            private_copy->boundary.excluded_aliases && private_copy->boundary.excluded_uavs && private_copy->boundary.excluded_writers[0] &&
            private_copy->boundary.excluded_writers[3] && !private_copy->boundary.excluded_writers[1] && !private_copy->boundary.excluded_writers[2],
            "actual wildcard alias, dispatch and unrelated UAV work cannot reach verified private non-UAV target; receipt retains all exclusions");
        require(probe.request_snapshot(51),"arm actual known indirect compute exclusion");private_generation(true);
        const auto indirect_compute_copy=probe.take_snapshot(51);
        require(indirect_compute_copy && indirect_compute_copy->result==S_OK && indirect_compute_copy->boundary.excluded_writers[1]==1 && indirect_compute_copy->boundary.indirect_exclusions[0]==1,
            "observed indirect compute signature excludes writes to private non-UAV target without assuming RTV state");
        require(probe.snapshot_status().indirect_actions[GpuIndirectEvidence::dispatch]==1 && probe.snapshot_status().signature &&
            probe.snapshot_status().signature->shape.action()==GpuIndirectEvidence::dispatch && probe.snapshot_status().indirect_max_count==1,
            "actual ExecuteIndirect is attributed to its observed dispatch signature");
        require(probe.request_snapshot(52),"arm unknown signature refusal");private_generation(true,unknown_signature.Get());
        require(!probe.take_snapshot(52) && (probe.snapshot_status().barrier_hazards&GpuStateEvidence::unsupported_writer),"unknown pre-observation signature still refuses on private target");
        private_generation(false);require(probe.take_snapshot(52).has_value(),"fresh known generation recovers from unknown signature without erasing old evidence");
        std::cout<<"private_access_runtime_cases_exercised=alias,dispatch,uav_clear,uav_barrier,known_indirect_compute,unknown_indirect_refusal\n";
        D3D12_INDIRECT_ARGUMENT_DESC draw_argument{};draw_argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC draw_signature_desc{};draw_signature_desc.ByteStride=sizeof(D3D12_DRAW_ARGUMENTS);draw_signature_desc.NumArgumentDescs=1;draw_signature_desc.pArgumentDescs=&draw_argument;
        ComPtr<ID3D12CommandSignature> draw_signature;check(host.device->CreateCommandSignature(&draw_signature_desc,nullptr,IID_PPV_ARGS(&draw_signature)));
        check(arguments->Map(0,nullptr,&mapped_arguments));const D3D12_DRAW_ARGUMENTS zero_draw{0,1,0,0};std::memcpy(mapped_arguments,&zero_draw,sizeof(zero_draw));arguments->Unmap(0,nullptr);
        const auto graphics_state=[&] {
            host.commands->SetPipelineState(pipeline.Get());host.commands->SetGraphicsRootSignature(root.Get());host.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VIEWPORT vp{0,0,2,2,0,1};const D3D12_RECT clip{0,0,2,2};host.commands->RSSetViewports(1,&vp);host.commands->RSSetScissorRects(1,&clip);
        };
        require(probe.request_snapshot(53),"arm known target-bound indirect raster control");host.begin();graphics_state();
        auto private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&private_transition);
        host.commands->OMSetRenderTargets(1,&alias_handle,TRUE,nullptr);host.commands->ExecuteIndirect(draw_signature.Get(),1,arguments.Get(),0,nullptr,0);host.execute();
        const auto possible_draw=probe.take_snapshot(53);
        require(possible_draw && possible_draw->result==S_OK && possible_draw->writer.kinds[6]>0 && !possible_draw->writer.kinds[0] && !possible_draw->writer.kinds[1] &&
            !possible_draw->boundary.raster_state && possible_draw->writer.origin && possible_draw->writer.origin->render_id==writer_origin->render_id,
            "a GPU-zero indirect draw is counted as a possible writer with exact origin, never an actual draw or raster-state inference");
        host.begin();private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON);host.commands->ResourceBarrier(1,&private_transition);host.execute();
        require(probe.request_snapshot(54),"arm ClearState binding reset control");host.begin();
        host.commands->OMSetRenderTargets(1,&alias_handle,TRUE,nullptr);host.commands->ClearState(nullptr);graphics_state();
        host.commands->ExecuteIndirect(draw_signature.Get(),1,arguments.Get(),0,nullptr,0);
        private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&private_transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);std::swap(private_transition.Transition.StateBefore,private_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&private_transition);host.execute();
        const auto cleared_binding=probe.take_snapshot(54);
        require(cleared_binding && cleared_binding->result==S_OK && cleared_binding->boundary.indirect_exclusions[1] && !cleared_binding->writer.kinds[6] && probe.snapshot_status().binding_calls[0],
            "actual ClearState unbinds target before indirect work and does not retain stale RTV attribution");
        require(probe.request_snapshot(55),"arm descriptor-unknown indirect refusal");targets.clear();host.begin();graphics_state();
        host.commands->OMSetRenderTargets(1,&alias_handle,TRUE,nullptr);host.commands->ExecuteIndirect(draw_signature.Get(),1,arguments.Get(),0,nullptr,0);
        host.device->CreateRenderTargetView(private_texture.Get(),nullptr,alias_handle);
        private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&private_transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);std::swap(private_transition.Transition.StateBefore,private_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&private_transition);host.execute();
        require(!probe.take_snapshot(55) && (probe.snapshot_status().barrier_hazards&GpuStateEvidence::unsupported_writer),"unknown RTV descriptors refuse even after later known clear/full transitions");
        private_generation(false);require(probe.take_snapshot(55).has_value(),"fresh known recording recovers from unknown binding");
        const auto binding_caps=probe.barrier_capabilities();
        if(binding_caps.list4_supported) {
            ComPtr<ID3D12GraphicsCommandList4> pass;check(host.commands.As(&pass));require(probe.request_snapshot(56),"arm implicit render-pass refusal");host.begin();
            private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&private_transition);
            D3D12_RENDER_PASS_RENDER_TARGET_DESC attachment{};attachment.cpuDescriptor=alias_handle;
            attachment.BeginningAccess.Type=D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;attachment.BeginningAccess.Clear.ClearValue.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            attachment.EndingAccess.Type=D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;pass->BeginRenderPass(1,&attachment,nullptr,D3D12_RENDER_PASS_FLAG_NONE);pass->EndRenderPass();
            host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);std::swap(private_transition.Transition.StateBefore,private_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&private_transition);host.execute();
            require(!probe.take_snapshot(56) && (probe.snapshot_status().barrier_hazards&GpuStateEvidence::implicit_binding) && probe.snapshot_status().binding_calls[1] && probe.snapshot_status().binding_calls[2],
                "real implicit pass writes remain sticky through ending pass and later explicit clear/transitions");
            private_generation(false);require(probe.take_snapshot(56).has_value(),"fresh generation recovers without erasing pass refusal");
            std::cout<<"implicit_render_pass_runtime_case_exercised=true\n";
        }
        require(probe.request_snapshot(57),"arm actual implementation coverage refusal");deny_binding_coverage=true;private_generation(false);deny_binding_coverage=false;
        require(!probe.take_snapshot(57) && probe.latest() && !probe.latest()->all_bindings_covered,"uncovered binding implementation prevents copy even after known full transitions");
        private_generation(false);require(probe.take_snapshot(57).has_value(),"actual covered generation recovers from implementation refusal");
        std::cout<<"indirect_binding_runtime_cases_exercised=potential_zero_draw,clear_state,unknown_descriptor,implicit_pass,implementation_coverage\n";
        const auto extended_caps=probe.extended_writer_status();
        require(extended_caps.known && extended_caps.hook_mask==(131u|(extended_caps.list1?4u:0u)|(extended_caps.list4?56u:0u)|(extended_caps.list6?64u:0u)),
            "all extended hooks supported by the actual command interfaces install");
        require(probe.request_snapshot(58),"arm unrelated private discard exclusion");host.begin();
        auto other_transition=barrier(other.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&other_transition);
        host.commands->DiscardResource(other.Get(),nullptr);std::swap(other_transition.Transition.StateBefore,other_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&other_transition);
        private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&private_transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);std::swap(private_transition.Transition.StateBefore,private_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&private_transition);host.execute();
        const auto unrelated_discard=probe.take_snapshot(58);
        require(unrelated_discard && unrelated_discard->result==S_OK && unrelated_discard->boundary.extended_exclusions[GpuTargetAccess::discard],
            "actual discard of another canonical resource cannot affect private committed target backing");
        require(probe.request_snapshot(59),"arm actual target discard refusal");host.begin();
        private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&private_transition);
        host.commands->DiscardResource(private_texture.Get(),nullptr);host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);
        std::swap(private_transition.Transition.StateBefore,private_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&private_transition);host.execute();
        require(!probe.take_snapshot(59) && (probe.snapshot_status().barrier_hazards&GpuStateEvidence::extended_writer),"actual target discard remains unsafe through later clear/full transitions");
        private_generation(false);require(probe.take_snapshot(59).has_value(),"fresh generation recovers from target discard without erasing refusal");
        D3D12_RESOURCE_DESC depth_desc=private_texture->GetDesc();depth_desc.Format=DXGI_FORMAT_D32_FLOAT;depth_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        ComPtr<ID3D12Resource> depth_texture;check(host.device->CreateCommittedResource(&default_heap,D3D12_HEAP_FLAG_NONE,&depth_desc,D3D12_RESOURCE_STATE_DEPTH_WRITE,nullptr,IID_PPV_ARGS(&depth_texture)));
        D3D12_DESCRIPTOR_HEAP_DESC depth_heap_desc{};depth_heap_desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;depth_heap_desc.NumDescriptors=1;
        ComPtr<ID3D12DescriptorHeap> depth_descriptors;check(host.device->CreateDescriptorHeap(&depth_heap_desc,IID_PPV_ARGS(&depth_descriptors)));
        const auto depth_handle=depth_descriptors->GetCPUDescriptorHandleForHeapStart();host.device->CreateDepthStencilView(depth_texture.Get(),nullptr,depth_handle);
        require(probe.request_snapshot(60),"arm actual depth clear exclusion");host.begin();host.commands->ClearDepthStencilView(depth_handle,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
        private_transition=barrier(private_texture.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_RENDER_TARGET);host.commands->ResourceBarrier(1,&private_transition);
        host.commands->ClearRenderTargetView(alias_handle,rgba,0,nullptr);std::swap(private_transition.Transition.StateBefore,private_transition.Transition.StateAfter);host.commands->ResourceBarrier(1,&private_transition);host.execute();
        const auto depth_clear=probe.take_snapshot(60);
        require(depth_clear && depth_clear->result==S_OK && depth_clear->boundary.extended_exclusions[GpuTargetAccess::depth],"actual depth clear cannot write a private non-DS render target");
        std::cout<<"extended_writer_runtime_cases_exercised=other_discard,target_discard_refusal,depth_clear; hook_mask="<<extended_caps.hook_mask<<'\n';
        require(probe.target(texture.Get()),"restore original pre-observation target after private access fixture");
        host.device->CreateRenderTargetView(texture.Get(),nullptr,alias_handle);
        change_origin=true;draw_generation();require(probe.latest() && !probe.latest()->origin,"one batch cannot claim a unique source when its observed writers use different camera jobs");change_origin=false;
        fail_origin=true;draw_generation();require(probe.latest() && !probe.latest()->origin,"camera attribution failure preserves the real draw but refuses frame evidence");fail_origin=false;
        writer_origin->render_world[12]+=1;draw_generation();require(!probe.latest()->origin,"mismatched source/render poses cannot become GPU writer provenance");writer_origin->render_world=writer_origin->source_world;
        clear_origin=true;draw_generation();require(!probe.latest(),"target invalidation during unlocked ownership resolution cannot publish stale command-generation evidence");
        require(!probe.snapshot_status().writes && probe.snapshot_status().state==UINT32_MAX && !probe.snapshot_status().barrier_calls &&
                !probe.snapshot_status().target_transitions && !probe.snapshot_status().foreign_submissions,"target invalidation clears stale snapshot diagnostics");
        require(probe.target(other.Get()) && !probe.latest(),"changing target invalidates completed evidence from the old resource");
        probe.clear_target();require(!probe.latest(),"device/reset invalidation removes acceptance evidence");
        std::cout<<"GPU completion probe tests passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<'\n';return 1;}
}
