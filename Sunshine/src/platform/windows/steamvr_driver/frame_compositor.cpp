// frame_compositor.cpp - 見 frame_compositor.h。
#include "frame_compositor.h"

#include <cmath>
#include <cstring>

#include "compositor_ps.h"  // fxc 產生（out\gen）
#include "compositor_vs.h"

namespace vrdrv {

  namespace {
    struct layer_cb_t {
      float dst_rect[4];
      float uv_rect[4];
      uint32_t mode;
      uint32_t force_alpha;
      uint32_t pad[2];
    };

    static_assert(sizeof(layer_cb_t) == 48, "cbuffer 大小必須是 16 的倍數");

    // layer 的投影矩陣 → tangent 範圍（l、r、t=down、b=up）。OpenVR 的 GetProjectionMatrix：
    // p00 = 2/(r−l)、p02 = (r+l)/(r−l)、p11 = 2/(b−t)、p12 = (b+t)/(b−t) → ndc = p·tan − p_2。
    // 影像的上方永遠是 up：解出來的上下／左右若反了就對調（不讓投影慣例的差異把畫面翻過來）。
    bool proj_to_rect(const float m[4][4], math::rect_t &out) {
      const float p00 = m[0][0], p02 = m[0][2], p11 = m[1][1], p12 = m[1][2];
      if (!(std::fabs(p00) > 1e-6f) || !(std::fabs(p11) > 1e-6f) || !std::isfinite(p00) || !std::isfinite(p02) || !std::isfinite(p11) || !std::isfinite(p12)) {
        return false;
      }
      float l = (p02 - 1.0f) / p00, r = (p02 + 1.0f) / p00;
      float t = (p12 - 1.0f) / p11, b = (p12 + 1.0f) / p11;
      if (l > r) {
        const float s = l;
        l = r;
        r = s;
      }
      if (t > b) {
        const float s = t;
        t = b;
        b = s;
      }
      out.left = l;
      out.right = r;
      out.top = t;
      out.bottom = b;
      return true;
    }
  }  // namespace

  HRESULT frame_compositor_t::init(ID3D11Device *dev) {
    HRESULT hr = dev->CreateVertexShader(g_compositor_vs, sizeof(g_compositor_vs), nullptr, &vs_);
    if (FAILED(hr)) {
      return hr;
    }
    hr = dev->CreatePixelShader(g_compositor_ps, sizeof(g_compositor_ps), nullptr, &ps_);
    if (FAILED(hr)) {
      return hr;
    }
    D3D11_SAMPLER_DESC sd {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    hr = dev->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr)) {
      return hr;
    }
    D3D11_BLEND_DESC bd {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = dev->CreateBlendState(&bd, &blend_opaque_);
    if (FAILED(hr)) {
      return hr;
    }
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    hr = dev->CreateBlendState(&bd, &blend_alpha_);
    if (FAILED(hr)) {
      return hr;
    }
    D3D11_RASTERIZER_DESC rd {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    hr = dev->CreateRasterizerState(&rd, &raster_);
    if (FAILED(hr)) {
      return hr;
    }
    D3D11_DEPTH_STENCIL_DESC dd {};
    dd.DepthEnable = FALSE;
    dd.StencilEnable = FALSE;
    hr = dev->CreateDepthStencilState(&dd, &depth_off_);
    if (FAILED(hr)) {
      return hr;
    }
    D3D11_BUFFER_DESC cbd {};
    cbd.ByteWidth = sizeof(layer_cb_t);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return dev->CreateBuffer(&cbd, nullptr, &cb_);
  }

  void frame_compositor_t::compose(ID3D11DeviceContext *ctx, ID3D11RenderTargetView *rtv, uint32_t eye_w, uint32_t eye_h, const math::rect_t eye_rect[2], const compose_layer_t *layers, uint32_t count) {
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(rtv, black);
    if (count == 0 || !ready()) {
      return;
    }
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetDepthStencilState(depth_off_.Get(), 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->RSSetState(raster_.Get());
    ID3D11SamplerState *s = sampler_.Get();
    ctx->PSSetSamplers(0, 1, &s);
    ID3D11Buffer *cb = cb_.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);

    for (uint32_t e = 0; e < 2; ++e) {
      const math::rect_t &E = eye_rect[e];
      const float ew = E.right - E.left;
      const float eh = E.bottom - E.top;
      if (!(ew > 1e-6f) || !(eh > 1e-6f)) {
        continue;
      }
      D3D11_VIEWPORT vp {};
      vp.TopLeftX = (float) (e * eye_w);
      vp.TopLeftY = 0.0f;
      vp.Width = (float) eye_w;
      vp.Height = (float) eye_h;
      vp.MaxDepth = 1.0f;
      ctx->RSSetViewports(1, &vp);
      for (uint32_t i = 0; i < count; ++i) {
        const compose_eye_t &L = layers[i].eye[e];
        if (!L.srv) {
          continue;
        }
        math::rect_t lr;
        if (!proj_to_rect(L.proj, lr)) {
          lr = E;
        }
        layer_cb_t c {};
        c.dst_rect[0] = (2.0f * lr.left - (E.right + E.left)) / ew;
        c.dst_rect[1] = (2.0f * lr.top - (E.bottom + E.top)) / eh;  // down → NDC 下緣
        c.dst_rect[2] = (2.0f * lr.right - (E.right + E.left)) / ew;
        c.dst_rect[3] = (2.0f * lr.bottom - (E.bottom + E.top)) / eh;  // up → NDC 上緣
        c.uv_rect[0] = L.bounds[0];
        c.uv_rect[1] = L.bounds[1];
        c.uv_rect[2] = L.bounds[2];
        c.uv_rect[3] = L.bounds[3];
        c.mode = L.mode;
        c.force_alpha = L.force_alpha ? 1u : 0u;
        D3D11_MAPPED_SUBRESOURCE m {};
        if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
          continue;
        }
        std::memcpy(m.pData, &c, sizeof(c));
        ctx->Unmap(cb, 0);
        ctx->OMSetBlendState(i == 0 ? blend_opaque_.Get() : blend_alpha_.Get(), nullptr, 0xFFFFFFFFu);
        ID3D11ShaderResourceView *srv = L.srv;
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->Draw(4, 0);
      }
    }
    ID3D11ShaderResourceView *null_srv = nullptr;
    ctx->PSSetShaderResources(0, 1, &null_srv);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
  }

}  // namespace vrdrv
