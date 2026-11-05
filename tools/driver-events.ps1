<#
.SYNOPSIS
Prints the display driver's latest messages from the Application event log, oldest first.

.DESCRIPTION
The driver logs under an unregistered event source. Get-WinEvent -ProviderName cannot find such a source and Event Viewer has
no description for it, so this filters by XPath and prints each event's first data item, which is the message.
#>
param([int]$Count = 20)

Get-WinEvent -LogName Application -FilterXPath "*[System/Provider[@Name='AndroidUsbMonitor']]" -MaxEvents $Count -ErrorAction SilentlyContinue |
    Sort-Object TimeCreated |
    ForEach-Object { "$($_.TimeCreated.ToString('HH:mm:ss.fff')) $($_.Properties[0].Value)" }
