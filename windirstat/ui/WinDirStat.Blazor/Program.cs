using WinDirStat.Blazor.Components;
using WinDirStat.Blazor.Rpc;
using WinDirStat.Blazor.Services;

var builder = WebApplication.CreateBuilder(args);

builder.Services.AddRazorComponents()
    .AddInteractiveServerComponents();

builder.Services.AddSingleton<MockScanFeed>();
builder.Services.AddSingleton<NamedPipeScanClient>();

var app = builder.Build();

if (!app.Environment.IsDevelopment())
{
    app.UseExceptionHandler("/Error");
}

app.UseAntiforgery();

app.MapRazorComponents<App>()
    .AddInteractiveServerRenderMode();

app.Run();
