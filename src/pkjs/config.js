module.exports = [
  {
    "type": "heading",
    "defaultValue": "FuelWatch Settings"
  },
  {
    "type": "section",
    "items": [
      {
        "type": "select",
        "messageKey": "FuelType",
        "defaultValue": "E10",
        "label": "Fuel type",
        "options": [
          { "label": "E10 (Euro 95)",  "value": "E10"    },
          { "label": "E5 (Super 98)",  "value": "E5"     },
          { "label": "Diesel (B7)",    "value": "DIESEL"  },
          { "label": "LPG / Autogas", "value": "LPG"    }
        ]
      }
    ]
  },
  {
    "type": "submit",
    "defaultValue": "Save"
  }
];