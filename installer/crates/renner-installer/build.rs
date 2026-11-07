fn main() {
    #[cfg(feature = "gui")]
    {
        let config = slint_build::CompilerConfiguration::new()
            .with_style("fluent-dark".into())
            // The plain Arch ISO has no desktop fonts: embed ours.
            .embed_resources(slint_build::EmbedResourcesKind::EmbedFiles);
        slint_build::compile_with_config("ui/installer.slint", config).expect("Slint UI failed to compile");
    }
}
