# assets - loaders by extension, cache by path, aliases

Header: `engine/include/braty/br_assets.h`. Source: `engine/src/assets/br_assets.c`.
Task 2 wrote the core below; Task 5 extends this module (resolver, manifests, bundles,
background loading, spritesheets, more loaders) and this guide.

## Functions you actually need

| Function | What for |
|---|---|
| `br_assets_register_loader(e, &desc)` | Handle file extensions (later registration wins). |
| `br_assets_load(e, path)` | Load or return cached; NULL on failure (path + reason logged). |
| `br_assets_get`, `br_assets_unload`, `br_assets_unload_all` | Cache access. |
| `br_assets_add_alias(e, "hero", "images/hero.png")` | Short names. |
| `br_assets_type(e, path)` | Loader type name of a cached asset ("texture"). |
| `br_texture_load(e, path)` | Typed wrapper: never fails, magenta fallback (br_texture.h). |

## Snippet (texture loader, engine/src/render/br_texture.c)

```c
br_asset_loader_desc desc;
SDL_zero(desc);
desc.type_name = "texture";
desc.extensions = "png,jpg,jpeg,bmp,tga,gif,psd,hdr";
desc.load = br_texture_asset_load;     /* (e, path, bytes, size) -> heap br_texture * */
desc.unload = br_texture_asset_unload;
br_assets_register_loader(e, &desc);
```

## Gotchas

- Paths are relative to the asset root (`br_engine_asset_root`), forward slashes.
- The loader gets the whole file in memory; it returns any pointer it likes (free it in unload).
- Failed loads are not cached (a later retry reads the file again).
- Shutdown order: assets unload before the render module frees textures (br_builtin.c order).
- Register loaders from your module's `br_<module>_register`; the asset core exists by then.
