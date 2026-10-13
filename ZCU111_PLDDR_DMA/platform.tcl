platform generate -domains 
platform generate -domains 
platform generate -domains standalone_psu_cortexa53_1 
platform generate -domains standalone_psu_cortexa53_1 
platform generate
platform generate
platform active {ZCU111_PLDDR_DMA}
bsp reload
bsp config tcp_wnd "65535"
bsp config tcp_snd_buf "65535"
bsp write
bsp reload
catch {bsp regenerate}
domain active {standalone_domain}
bsp reload
bsp reload
domain active {standalone_psu_cortexa53_1}
bsp config tcp_snd_buf "16384"
bsp config tcp_wnd "16384"
bsp write
bsp reload
catch {bsp regenerate}
platform generate
platform clean
platform generate
bsp config tcp_snd_buf "65535"
bsp config tcp_wnd "65535"
bsp write
bsp reload
catch {bsp regenerate}
platform clean
platform clean
bsp config tcp_wnd "16384"
bsp write
bsp reload
catch {bsp regenerate}
platform clean
bsp config tcp_wnd "16384"
bsp config tcp_snd_buf "16384"
bsp write
bsp reload
catch {bsp regenerate}
platform clean
platform generate
platform clean
platform generate
bsp config tcp_wnd "16384"
bsp config tcp_snd_buf "65535"
bsp write
bsp reload
catch {bsp regenerate}
platform clean
platform clean
bsp config memp_n_tcp_seg "1024"
bsp config mem_size "262144"
bsp write
bsp reload
catch {bsp regenerate}
platform generate
platform clean
platform generate
platform active {ZCU111_PLDDR_DMA}
bsp reload
bsp config tcp_wnd "65535"
bsp write
bsp reload
catch {bsp regenerate}
platform generate -domains standalone_psu_cortexa53_1 
domain active {standalone_domain}
bsp reload
bsp reload
platform clean
platform generate
domain active {standalone_psu_cortexa53_1}
bsp reload
