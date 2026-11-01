        // Actual MinHook callback inside the actual dispatcher/two-Scene path.
        // GPU APIs are not called: this native fixture only checks exact ABI.
        {
            using ClearProbe=NativeSceneClearProbe;auto& clears=ClearProbe::get();
            std::array<uint8_t,0x300> clear_pe{};const uintptr_t clear_image=0x10000000;auto clear_sig=SYNTH(ClearProbe::SIGNATURE);uintptr_t clear_callback=clear_image+0x213dfe0;
            const auto peput=[&](size_t p,const auto& v){std::memcpy(clear_pe.data()+p,&v,sizeof(v));};
            peput(0,uint16_t{0x5a4d});peput(0x3c,uint32_t{0x80});peput(0x80,uint32_t{0x4550});peput(0x84,uint16_t{0x8664});peput(0x86,uint16_t{1});peput(0x88,uint32_t{0x6ac3e716});peput(0x94,uint16_t{240});peput(0x98,uint16_t{0x20b});peput(0xd0,uint32_t{0x3800000});peput(0x190,uint32_t{0x20000});peput(0x194,uint32_t{0x2130000});peput(0x1ac,uint32_t{0x20000000});
            const auto image_read=[&](uintptr_t p,void* out,size_t n){if(p>=clear_image && p-clear_image<clear_pe.size() && n<=clear_pe.size()-(p-clear_image)){std::memcpy(out,clear_pe.data()+p-clear_image,n);return true;}if(p==clear_image+0x213dfe0 && n==clear_sig.size()){std::memcpy(out,clear_sig.data(),n);return true;}if(p==clear_image+0x37d7d80+0x158 && n==8){std::memcpy(out,&clear_callback,8);return true;}return false;};
            require(ClearProbe::supported(clear_image,image_read),"shipping image stamp/executable callback/signature/vtable slot qualify clear implementation");
            clear_sig[0]^=1;require(!ClearProbe::supported(clear_image,image_read),"wrong clear function bytes refuse installation");clear_sig[0]^=1;
            clear_callback+=16;require(!ClearProbe::supported(clear_image,image_read),"wrong actual native command clear slot refuses installation");clear_callback-=16;
            clear_pe[0x88]^=1;require(!ClearProbe::supported(clear_image,image_read),"unsupported build stamp refuses clear layout");clear_pe[0x88]^=1;
            std::array<uintptr_t,44> clear_vtable{};std::vector<uint8_t> clear_command(0x12000);std::array<uint8_t,0x280> clear_wrapper{};std::array<uint8_t,0x20> clear_values{};
            const auto cp=reinterpret_cast<uintptr_t>(clear_command.data()),wp=reinterpret_cast<uintptr_t>(clear_wrapper.data()),vp=reinterpret_cast<uintptr_t>(clear_values.data()),vt=reinterpret_cast<uintptr_t>(clear_vtable.data());
            const auto cput=[&](size_t offset,const auto& value){std::memcpy(clear_command.data()+offset,&value,sizeof(value));};
            cput(0,vt);cput(0xfdc8,wp);cput(0x1194c,uint8_t{1});cput(0x11950,uintptr_t{0xd500});cput(0xfe90,uint32_t{0x3f800000});cput(0xfe94,uint8_t{7});
            const uintptr_t resource=0x123450;std::memcpy(clear_wrapper.data()+0x278,&resource,8);const uint32_t explicit_depth=0x3e800000;std::memcpy(clear_values.data()+0x10,&explicit_depth,4);clear_values[0x14]=9;
            require(clears.install(reinterpret_cast<void*>(&scene_clear_fixture),vt),"actual native clear hook installs");
            clears.m_reader=scene_clear_reader;clears.m_now=scene_clear_clock;scene_clear_now=ClearProbe::Clock::now();
            const auto saved_count=probe.m_count;unsigned expected_calls{};
            auto run_clear=[&](const std::string& name,const std::function<void(int32_t)>& actions){
                const auto file=directory/(name+".draw_visibility.json");std::filesystem::remove(file);std::ofstream(request)<<name<<'\n';probe.m_next={};probe.m_count=0;
                scene_steps=[&](void* r,uint32_t,int32_t slot){const auto entry=actual[slot].address;std::memcpy(static_cast<uint8_t*>(r)+0x140,&entry,8);actions(slot);};
                actual_dispatch(renderer.data(),0);return file;
            };
            scene_clear_steps=[&](uintptr_t command,uint32_t f,uintptr_t values){require(command==cp && (f==2 || f==6) && values==(f==2?0:vp) && scene_clear_native_incoming==0x13572468,"clear forwards exact command, full flags, optional pointer and incoming native error");};
            auto file=run_clear("clear_two_scene",[&](int32_t slot){SetLastError(0x13572468);scene_clear_call(cp,slot?6:2,slot?vp:0);++expected_calls;require(GetLastError()==0xc1ea1234,"clear preserves original outgoing LastError");});
            require(scene_clear_native_calls==expected_calls && scene_clear_native_incoming==0x13572468,"actual clear callback executes exactly once per observed invocation");
            auto clear_frame=probe.m_frame;require(clear_frame && clear_frame->clears && clear_frame->clears->count==2,"named actual two-Scene capture retains both clear calls");
            auto session=clear_frame->clears;
            require(session->events[0].slot==0 && session->events[1].slot==1 && session->events[0].ordinal==1 && session->events[1].ordinal==2 && session->events[0].dispatch==session->events[1].dispatch && session->events[0].render==counter,"clear evidence belongs to actual Scene dispatch/render/ordinal/slot, not raw selector contexts");
            require(session->events[0].same_call && session->events[1].same_call && !session->events[0].before.mask && !session->events[1].after.mask && session->events[0].before.depth_word==0x3f800000 && session->events[1].before.depth_word==explicit_depth && session->events[1].after.stencil==9 && session->events[0].before.resource==resource,"stable default and explicit native clear values and target resource are separately observed");
            std::ifstream clear_saved{file};std::string clear_json{std::istreambuf_iterator<char>{clear_saved},{}};clear_saved.close();
            require(clear_json.find("\"clear_events\":{\"model\":\"ordinary_scene_clear_events_v1\"")!=clear_json.npos && clear_json.find("\"absence_verified\":false")!=clear_json.npos && clear_json.find("\"depth_eye_ownership_verified\":false")!=clear_json.npos,"clear receipt retains absence and resource-eye ownership limits");std::filesystem::remove(file);
            scene_clear_steps={};
            file=run_clear("clear_refusals",[&](int32_t slot){
                if(slot)return;const auto held=probe.m_frame->clears;
                // Same native Scene TLS but wrong active renderer source.
                uintptr_t wrong=1;std::memcpy(renderer.data()+0x140,&wrong,8);scene_clear_call(cp,4,0);++expected_calls;std::memcpy(renderer.data()+0x140,&actual[0].address,8);
                require(!held->events[0].identity_before && held->events[0].before.mask==ClearProbe::ownership,"unowned active entry refuses source-labelled clear metadata");
                cput(0,uintptr_t{1});scene_clear_call(cp,1,0);++expected_calls;cput(0,vt);require(held->events[1].before.mask==ClearProbe::unsupported,"unknown command backend remains unknown without interpretation");
                unsigned sources{};scene_clear_read_steps=[&](uintptr_t p,void* out,size_t n){SIZE_T got{};const auto ok=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&got) && got==n;if(p==cp+0xfdc8 && ++sources==1)cput(0xfdc8,uintptr_t{});return ok;};
                scene_clear_call(cp,6,0);++expected_calls;scene_clear_read_steps={};cput(0xfdc8,wp);require(held->events[2].before.mask==ClearProbe::changed,"torn consumed depth wrapper cannot produce stable target metadata");
                scene_clear_steps=[&](uintptr_t,uint32_t,uintptr_t){cput(0x11950,uintptr_t{0xd501});};scene_clear_call(cp,2,0);++expected_calls;scene_clear_steps={};
                require(held->events[3].same_call && held->events[3].before.dsv==0xd500 && held->events[3].after.dsv==0xd501,"per-call before and after may legitimately differ while individually stable");
                cput(0x1194c,uint8_t{});scene_clear_call(cp,1,0);++expected_calls;require(!held->events[4].before.present && held->events[4].flags==1,"colour-only request with absent depth is retained without inventing a depth clear");cput(0x1194c,uint8_t{1});
                unsigned vtable_reads{};scene_clear_read_steps=[&](uintptr_t p,void* out,size_t n){SIZE_T got{};const auto ok=ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),out,n,&got) && got==n;if(p==cp && ++vtable_reads==1)cput(0,uintptr_t{1});return ok;};
                scene_clear_call(cp,2,0);++expected_calls;scene_clear_read_steps={};cput(0,vt);require(held->events[5].before.mask==ClearProbe::unsupported,"vtable becoming stably unsupported after initial qualification is not a valid target snapshot");
            });std::filesystem::remove(file);
            file=run_clear("clear_nested",[&](int32_t slot){if(slot)return;bool nested{};scene_clear_steps=[&](uintptr_t,uint32_t,uintptr_t){if(nested)return;nested=true;cput(0x11950,uintptr_t{0x222});scene_clear_call(cp,4,vp);++expected_calls;cput(0x11950,uintptr_t{0x333});};
                scene_clear_call(cp,2,0);++expected_calls;scene_clear_steps={};const auto& e=probe.m_frame->clears->events;
                require(probe.m_frame->clears->count==2 && e[0].serial==1 && e[1].serial==2 && e[0].after.dsv==0x333 && e[1].before.dsv==0x222 && e[1].after.dsv==0x222,"nested native clears retain each call's own immutable event index and target snapshots");
            });std::filesystem::remove(file);
            file=run_clear("clear_reentrant",[&](int32_t slot){if(slot)return;const auto old_address=probe.m_frame.get();std::weak_ptr<Probe::Frame> lifetime=probe.m_frame;const auto old_session=probe.m_frame->clears;const auto original_scope=NativeRenderProbe::current_scene();require(bool(original_scope),"real Scene TLS available for reentrant test");
                bool entered{};scene_clear_steps=[&](uintptr_t,uint32_t,uintptr_t){if(entered)return;entered=true;auto other=*original_scope;other.dispatch_sequence+=100;std::ofstream(request)<<"clear_reentrant_child\n";probe.m_next={};
                    {Probe::Scope nested_scope{other,pair};require(Probe::active() && probe.m_frame.get()!=old_address,"nested named frame replaces pending global frame");scene_clear_call(cp,4,0);++expected_calls;require(probe.m_frame->clears->events[0].before.mask==ClearProbe::ownership,"synthetic nested Scope cannot relabel actual current Scene TLS");Probe::describe(*Probe::s_eye,descriptor_pointer,false);nested_scope.finish();}
                    require(!lifetime.expired(),"outer Scope retains its Frame after reentrant replacement");
                };
                scene_clear_call(cp,2,0);++expected_calls;scene_clear_steps={};require(old_session->count==1 && old_session->events[0].same_call,"outer native clear post snapshot stays in original held session after nested Frame replacement");
            });std::filesystem::remove(file);std::filesystem::remove(directory/"clear_reentrant_child.draw_visibility.json");
            file=run_clear("clear_limits",[&](int32_t slot){if(slot)return;const auto held=probe.m_frame->clears;
                for(size_t n=0;n<ClearProbe::EVENT_LIMIT+2;++n){scene_clear_call(cp,2,0);++expected_calls;}
                require(held->count==ClearProbe::EVENT_LIMIT && held->dropped==2 && held->exhausted && held->reads<=ClearProbe::READ_LIMIT && held->bytes<=ClearProbe::BYTE_LIMIT,"event and read work are globally bounded while every native call continues");
            });std::filesystem::remove(file);
            file=run_clear("clear_expiry",[&](int32_t slot){if(slot)return;auto held=probe.m_frame->clears;scene_clear_steps=[&](uintptr_t,uint32_t,uintptr_t){scene_clear_now=held->until;};scene_clear_call(cp,2,0);++expected_calls;scene_clear_steps={};require(held->timed_out && !held->events[0].same_call && !held->events[0].identity_after,"expiry during native call refuses post ownership qualification");scene_clear_now=ClearProbe::Clock::now();});std::filesystem::remove(file);
            // Without a named Scope there is no observation, even if native runs.
            const auto prior=session->count;SetLastError(0x77778888);scene_clear_call(cp,0xfedcba98,vp);++expected_calls;
            require(session->count==prior && GetLastError()==0xc1ea1234 && scene_clear_native_incoming==0x77778888 && scene_clear_native_calls==expected_calls,"unarmed clear calls remain native exactly once with all flag bits and errors preserved");
            // The only remaining strong Frame owner can be the Scope itself.
            // Releasing its optional passive members must not change game errors.
            std::ofstream(request)<<"clear_deleter\n";probe.m_next={};probe.m_count=0;auto deleter_scene=scene;deleter_scene.slot=0;deleter_scene.ordinal=1;deleter_scene.entry=actual[0].address;std::memcpy(renderer.data()+0x140,&actual[0].address,8);bool deleted{};
            {Probe::Scope owned{deleter_scene,pair};require(Probe::active(),"last-owner destructor fixture admits named Scope");probe.m_frame->clears=std::shared_ptr<ClearProbe::Session>(new ClearProbe::Session,[&](ClearProbe::Session* s){delete s;deleted=true;SetLastError(0xbadbad);});probe.m_frame.reset();SetLastError(0x89898989);}
            require(deleted && GetLastError()==0x89898989,"last-owner Scope destruction preserves error across custom session deleter");
            clears.m_now=ClearProbe::Clock::now;clears.m_reader=ClearProbe::memory;scene_clear_steps={};scene_clear_read_steps={};probe.m_count=saved_count;
            std::filesystem::remove(request);probe.m_frame.reset();
        }
