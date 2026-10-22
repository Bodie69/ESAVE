# R8 rules for release builds.
# Custom views used from XML layouts (EnergyChartView, EnergyHistoryChartView) and the
# manifest components are kept automatically by AAPT-generated rules. The app has no
# reflection-based code, so no extra keep rules are needed.

# Keep line numbers so release crash stack traces stay readable.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile
