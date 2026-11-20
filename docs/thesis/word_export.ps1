# Opens thesis.docx in Word (hidden), fills the contents field, saves it, and exports thesis.pdf;
# prints the page and word counts. Called by build.sh on Windows/WSL when Word is installed.
param([string]$Docx, [string]$Pdf)
$w = New-Object -ComObject Word.Application
$w.Visible = $false
$w.DisplayAlerts = 0
try {
  $d = $w.Documents.Open($Docx, $false, $false)
  $d.TablesOfContents | ForEach-Object { $_.Update() }
  "pages: " + $d.ComputeStatistics(2) + ", words: " + $d.ComputeStatistics(0)
  $d.Save()
  $d.SaveAs2($Pdf, 17)
  $d.Close($false)
} finally { $w.Quit() }
