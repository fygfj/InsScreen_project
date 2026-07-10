param(
    [Parameter(Mandatory = $false)]
    [string]$Token = $env:GITEE_TOKEN,

    [Parameter(Mandatory = $false)]
    [string]$Owner = "gxp666111",

    [Parameter(Mandatory = $false)]
    [string]$Repo = "miaomiao"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Token)) {
    throw "Missing Gitee token. Set `$env:GITEE_TOKEN first, then rerun this script."
}

$AboutPath = Join-Path (Split-Path -Parent $PSScriptRoot) "docs\gitee-about.md"
$About = Get-Content -LiteralPath $AboutPath -Encoding UTF8 -Raw

function Get-FencedBlock {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Text,

        [Parameter(Mandatory = $true)]
        [string]$InfoString
    )

    $Pattern = '```' + [regex]::Escape($InfoString) + '\s*(.*?)\s*```'
    $Match = [regex]::Match($Text, $Pattern,
                            [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if ($Match.Success) {
        return $Match.Groups[1].Value.Trim()
    }
    return $null
}

$Description = Get-FencedBlock -Text $About -InfoString "gitee-description"
$Homepage = Get-FencedBlock -Text $About -InfoString "gitee-homepage"

if ([string]::IsNullOrWhiteSpace($Description)) {
    $Matches = [regex]::Matches($About, '```text\s*(.*?)\s*```',
                                [System.Text.RegularExpressions.RegexOptions]::Singleline)
    if ($Matches.Count -lt 1) {
        throw "Could not find gitee-description block in docs/gitee-about.md"
    }
    $Description = $Matches[0].Groups[1].Value.Trim()
}

if ([string]::IsNullOrWhiteSpace($Homepage)) {
    $Homepage = "https://oshwhub.com/team_voosogmo/project_fxbcjhaa"
}

$Body = @{
    access_token = $Token
    description  = $Description
    homepage     = $Homepage
} | ConvertTo-Json -Compress

$Uri = "https://gitee.com/api/v5/repos/$Owner/$Repo"
$Resp = Invoke-RestMethod -Uri $Uri -Method Patch -ContentType "application/json; charset=utf-8" -Body $Body

[pscustomobject]@{
    ok          = $true
    full_name   = $Resp.full_name
    description = $Resp.description
    homepage    = $Resp.homepage
}
