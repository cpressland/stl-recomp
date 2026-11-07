# Direct3D 9, as the game uses it

What Phase 4's Metal device has to answer, measured: a headless run of the
1.2 game (`stl --run --headless --mute --watchdog 180`, 2026-11-07) on
Phase 3's device that draws nothing, from start-up into its main loop. The
game was given no input, so this is start-up and the screens it reaches on
its own; a mission will add more.

**Shaders** (`STL_DUMP_SHADERS`): D3DX compiles the game's effects at start,
and the first mission adds a few: 97 different shaders.

| Model | Count |
|---|---|
| `ps_1_1` | 16 |
| `ps_2_0` | 32 |
| `ps_2_x` | 2 |
| `vs_1_1` | 45 |
| `vs_2_0` | 2 |

125 KB of bytecode in all, the largest 3.7 KB. (A first count, 94 at
start-up, was of shaders cut short at their constant table, a bug since
fixed: the table can hold the end token's value.) No Shader Model 3 at the
default settings. Fixed-function stages are set too (`SetTextureStageState`,
`SetLight`, `SetMaterial`), so fixed function is needed alongside.

**Calls**, 180 seconds, 123,035 frames presented (about 680 a second with
nothing drawn and no wait for the display):

```
    44407153  IDirect3DDevice9::SetRenderState
    20147457  IDirect3DDevice9::SetSamplerState
     4029295  IDirect3DDevice9::SetTexture
     4029166  IDirect3DDevice9::SetVertexShader
     4029160  IDirect3DDevice9::SetPixelShader
     4028872  IDirect3DDevice9::SetVertexDeclaration
     3188924  IDirect3DDevice9::DrawPrimitiveUP
     1679390  IDirect3DDevice9::SetStreamSource
     1679390  IDirect3DDevice9::SetIndices
      839718  IDirect3DVertexBuffer9::Lock
      839718  IDirect3DIndexBuffer9::Lock
      839718  IDirect3DVertexBuffer9::Unlock
      839718  IDirect3DIndexBuffer9::Unlock
      839695  IDirect3DDevice9::DrawIndexedPrimitive
      476525  IDirect3DDevice9::SetViewport
      365850  IDirect3DDevice9::EndScene
      365850  IDirect3DDevice9::BeginScene
      132643  IDirect3DTexture9::GetSurfaceLevel
      126291  IDirect3DDevice9::Clear
      126290  IDirect3DDevice9::StretchRect
      123035  IDirect3DDevice9::Present
       29295  IDirect3DDevice9::SetDepthStencilSurface
       29295  IDirect3DDevice9::SetRenderTarget
       24844  IDirect3DSurface9::GetDesc
       22983  IDirect3DDevice9::SetPixelShaderConstantF
        6626  IDirect3DTexture9::GetLevelDesc
        5056  IDirect3DTexture9::UnlockRect
        5056  IDirect3DTexture9::LockRect
        4865  IDirect3D9::CheckDeviceFormat
        1734  IDirect3DStateBlock9::Capture
        1600  IDirect3DSurface9::LockRect
        1600  IDirect3DSurface9::UnlockRect
        1452  IDirect3DTexture9::GetLevelCount
        1318  IDirect3DStateBlock9::Apply
        1250  IDirect3DDevice9::EndStateBlock
        1250  IDirect3DDevice9::BeginStateBlock
         756  IDirect3D9::CheckDeviceMultiSampleType
         523  IDirect3DDevice9::SetVertexShaderConstantF
         494  IDirect3DDevice9::GetDeviceCaps
         448  IDirect3DDevice9::SetTextureStageState
         384  IDirect3DDevice9::LightEnable
         192  IDirect3DDevice9::SetTransform
         162  IDirect3DVertexShader9::GetFunction
         162  IDirect3DPixelShader9::GetFunction
         160  IDirect3DDevice9::SetLight
         155  IDirect3DDevice9::CreateTexture
         146  IDirect3DDevice9::GetDisplayMode
         146  IDirect3DDevice9::GetDirect3D
         145  IDirect3D9::CheckDepthStencilMatch
         118  IDirect3DDevice9::CreateVertexDeclaration
         113  IDirect3DDevice9::ValidateDevice
          96  IDirect3DCubeTexture9::GetCubeMapSurface
          81  IDirect3DVertexShader9::GetDevice
          81  IDirect3DPixelShader9::GetDevice
          81  IDirect3DDevice9::CreateVertexShader
          81  IDirect3DDevice9::CreatePixelShader
          72  IDirect3D9::CheckDeviceType
          64  IDirect3DDevice9::SetMaterial
          46  IDirect3D9::EnumAdapterModes
          28  IDirect3DDevice9::CreateVertexBuffer
          27  IDirect3DDevice9::CreateIndexBuffer
          24  IDirect3DTexture9::GetType
          18  IDirect3DCubeTexture9::GetLevelDesc
          18  IDirect3DCubeTexture9::GetLevelCount
           4  IDirect3D9::GetAdapterModeCount
           3  IDirect3D9::GetAdapterDisplayMode
           3  IDirect3D9::GetDeviceCaps
           2  IDirect3DDevice9::CreateCubeTexture
           1  IDirect3D9::GetAdapterIdentifier
           1  IDirect3D9::GetAdapterCount
           1  IDirect3DDevice9::SetFVF
           1  IDirect3D9::GetAdapterMonitor
           1  IDirect3DVolumeTexture9::GetVolumeLevel
           1  IDirect3DDevice9::CreateVolumeTexture
           1  IDirect3DDevice9::GetBackBuffer
           1  IDirect3D9::CreateDevice
           1  IDirect3DDevice9::CreateOffscreenPlainSurface
           1  IDirect3DDevice9::CreateRenderTarget
           1  IDirect3DDevice9::CreateDepthStencilSurface
           1  IDirect3DDevice9::GetDepthStencilSurface
           1  IDirect3DDevice9::GetRenderTarget
           1  IDirect3DDevice9::SetGammaRamp
```

Read as a to-do list: almost all of the drawing is `DrawPrimitiveUP` (user
pointer vertices, 3.2 M) and `DrawIndexedPrimitive` from buffers locked
every time (840 K), state set per draw (44 M render-state, 20 M sampler-state
calls: the Metal side wants them cached, not turned into pipeline states one
by one), render targets switched about every 4 frames, `StretchRect` once
per frame (the post-process chain), and state blocks only at start-up.
